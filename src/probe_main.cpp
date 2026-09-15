// st2022_delayprobe -- measure how long the test signal takes to get from one
// point to another, by matching frame numbers (and flashes) between sources,
// and the lip sync of each source, by pairing its flash with its tone mute.
//
//   st2022_delayprobe --source in=st2022:239.1.5.5@eth1,239.2.5.5@eth2
//                     --source out=sdi:0 --offset out=2
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "pcapreplay/net_interfaces.h"
#include "pcapreplay/net_multicast.h"
#include "probe/analyzer.h"
#include "probe/sdi_capture.h"
#include "probe/st2022_receiver.h"

using namespace pcapreplay;
using namespace testsignal;

namespace {

std::atomic<bool> g_stop{false};
extern "C" void onSignal(int) { g_stop.store(true); }

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (;;) {
        const std::size_t at = s.find(sep, start);
        out.push_back(s.substr(start, at - start));
        if (at == std::string::npos) return out;
        start = at + 1;
    }
}

std::string resolveIface(const std::string& given, std::string& error) {
    if (given.empty()) return {};
    NetInterface ni;
    if (findInterfaceByIp(given, ni)) return ni.ipv4;
    if (findInterfaceByName(given, ni) && !ni.ipv4.empty()) return ni.ipv4;
    error = "no usable interface called '" + given + "'";
    return {};
}

struct SourceSpec {
    std::string name;
    enum class Kind { St2022, Sdi } kind = Kind::St2022;
    St2022SourceConfig st2022;
    SdiSourceConfig sdi;
    double offsetFrames = 0.0;
    Region region;
};

// st2022:GROUP[:PORT][@IFACE][,GROUP[:PORT][@IFACE]]   or   sdi:DEVICE[:MODE]
bool parseSpec(const std::string& text, int defaultPort, SourceSpec& spec, std::string& error) {
    const std::size_t eq = text.find('=');
    if (eq == std::string::npos) { error = "--source wants NAME=SPEC"; return false; }
    spec.name = text.substr(0, eq);
    const std::string body = text.substr(eq + 1);
    if (body.rfind("st2022:", 0) == 0) {
        spec.kind = SourceSpec::Kind::St2022;
        const auto legs = split(body.substr(7), ',');
        if (legs.empty() || legs.size() > 2) { error = "st2022 wants one or two legs"; return false; }
        for (std::size_t i = 0; i < legs.size(); ++i) {
            St2022Leg leg;
            std::string rest = legs[i];
            const std::size_t at = rest.find('@');
            if (at != std::string::npos) {
                leg.interfaceIp = resolveIface(rest.substr(at + 1), error);
                if (!error.empty()) return false;
                rest = rest.substr(0, at);
            }
            const std::size_t colon = rest.find(':');
            leg.port = std::uint16_t(colon == std::string::npos ? defaultPort : std::atoi(rest.c_str() + colon + 1));
            leg.group = rest.substr(0, colon);
            if (!isValidMulticastGroup(leg.group)) { error = leg.group + " is not a multicast group"; return false; }
            if (i == 0) spec.st2022.a = leg;
            else { spec.st2022.b = leg; spec.st2022.haveB = true; }
        }
        return true;
    }
    if (body.rfind("sdi:", 0) == 0) {
        spec.kind = SourceSpec::Kind::Sdi;
        const auto parts = split(body.substr(4), ':');
        spec.sdi.device = std::atoi(parts[0].c_str());
        if (parts.size() > 1 && !parts[1].empty()) spec.sdi.mode = parts[1];
        return true;
    }
    error = "source spec must start st2022: or sdi:";
    return false;
}

void usage() {
    std::printf(
"st2022_delayprobe - measure test-signal delay and lip sync\n"
"\n"
"usage: st2022_delayprobe --source NAME=SPEC [--source NAME=SPEC ...] [options]\n"
"\n"
"With one source it reports that source's lip sync. With two or more it also\n"
"reports each source's delay from the reference.\n"
"\n"
"Sources\n"
"  --source NAME=st2022:GROUP[:PORT][@IFACE][,GROUP[:PORT][@IFACE]]\n"
"                         ST 2022-6, or ST 2022-7 with two legs\n"
"  --source NAME=sdi:DEVICE[:MODE]\n"
"                         DeckLink SDI input (MODE e.g. 1080p50; default auto)\n"
"  --ref NAME             the reference source (default: the first given)\n"
"  --offset NAME=FRAMES   subtract a device's own delay, in that source's frames\n"
"                         (a DeckLink reports 2)\n"
"  --region NAME=X,Y,W,H  where the test picture sits in that source's frame\n"
"                         (default: found from the first flash)\n"
"  --port N               default ST 2022-6 port (40000)\n"
"\n"
"Output\n"
"  --interval S           seconds between reports (default 2)\n"
"  --seconds N            stop after N seconds\n"
"  --csv FILE             one row per frame and per mute, per source\n"
"\n"
"Delay is arrival at a source minus arrival of the same frame number at the\n"
"reference, minus the source's offset. Lip sync is a source's tone mute minus\n"
"its flash; positive means the audio is late. Everything is stamped on this\n"
"machine's monotonic clock, so the sender's clock does not enter into it.\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> sourceArgs;
    std::map<std::string, std::string> offsetArgs, regionArgs;
    std::string refName, csvPath;
    int port = 40000;
    double interval = 2.0, seconds = 0.0;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        auto keyed = [&](std::map<std::string, std::string>& m) {
            const std::string v = val();
            const std::size_t eq = v.find('=');
            if (eq == std::string::npos) return false;
            m[v.substr(0, eq)] = v.substr(eq + 1);
            return true;
        };
        if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (a == "--source") sourceArgs.push_back(val());
        else if (a == "--ref") refName = val();
        else if (a == "--offset") { if (!keyed(offsetArgs)) { std::printf("--offset wants NAME=FRAMES\n"); return 2; } }
        else if (a == "--region") { if (!keyed(regionArgs)) { std::printf("--region wants NAME=X,Y,W,H\n"); return 2; } }
        else if (a == "--port") port = std::atoi(val().c_str());
        else if (a == "--interval") interval = std::atof(val().c_str());
        else if (a == "--seconds") seconds = std::atof(val().c_str());
        else if (a == "--csv") csvPath = val();
        else { std::printf("unknown option %s   (--help)\n", a.c_str()); return 2; }
    }
    if (sourceArgs.empty()) { usage(); return 2; }

    WinsockScope sockets;
    std::vector<SourceSpec> specs;
    for (const auto& text : sourceArgs) {
        SourceSpec spec;
        std::string error;
        if (!parseSpec(text, port, spec, error)) { std::printf("--source %s: %s\n", text.c_str(), error.c_str()); return 2; }
        if (offsetArgs.count(spec.name)) spec.offsetFrames = std::atof(offsetArgs[spec.name].c_str());
        if (regionArgs.count(spec.name)) {
            const auto v = split(regionArgs[spec.name], ',');
            if (v.size() != 4) { std::printf("--region wants X,Y,W,H\n"); return 2; }
            spec.region = {std::atoi(v[0].c_str()), std::atoi(v[1].c_str()), std::atoi(v[2].c_str()), std::atoi(v[3].c_str())};
        }
        specs.push_back(spec);
    }
    if (refName.empty()) refName = specs.front().name;
    if (specs.size() == 1) refName.clear();   // lip sync only

    std::FILE* csv = nullptr;
    if (!csvPath.empty() && !(csv = std::fopen(csvPath.c_str(), "w"))) {
        std::printf("cannot write %s\n", csvPath.c_str());
        return 2;
    }

    struct sigaction sa {};
    sa.sa_handler = onSignal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    Correlator correlator(refName, csv);
    struct Running {
        SourceSpec spec;
        std::unique_ptr<Analyzer> analyzer;
        MuteDetector mute;
        std::unique_ptr<St2022Receiver> st2022;
        std::unique_ptr<SdiCapture> sdi;
        std::mutex mutex, audioMutex;
        std::string format;
        std::uint64_t frames = 0, markers = 0, flashes = 0, mutes = 0;
    };
    std::vector<std::unique_ptr<Running>> running;
    std::atomic<int> refW{0}, refH{0};

    for (const auto& spec : specs) {
        auto r = std::make_unique<Running>();
        r->spec = spec;
        r->analyzer = std::make_unique<Analyzer>(spec.region);
        Running* rp = r.get();
        const bool isRef = spec.name == refName;
        FrameSink frames = [rp, isRef, &correlator, &refW, &refH](LumaFrame& f) {
            std::lock_guard<std::mutex> lk(rp->mutex);
            if (isRef) { refW.store(f.width); refH.store(f.height); }
            else if (refW.load() > 0) rp->analyzer->setSourceRaster(refW.load(), refH.load());
            const Observation o = rp->analyzer->analyse(f);
            ++rp->frames;
            if (o.frameNumber >= 0) ++rp->markers;
            if (o.flashStart) ++rp->flashes;
            rp->format = f.format;
            correlator.observe(rp->spec.name, o, rp->spec.offsetFrames);
        };
        AudioSink audio = [rp, &correlator](AudioBlock& b) {
            std::lock_guard<std::mutex> lk(rp->audioMutex);
            std::vector<std::int64_t> starts;
            rp->mute.feed(b, starts);
            for (const std::int64_t t : starts) {
                ++rp->mutes;
                correlator.observeMute(rp->spec.name, t);
            }
            if (rp->mute.hearingTone()) correlator.noteAudio(rp->spec.name, rp->mute.toneDbfs());
        };
        std::string error;
        if (spec.kind == SourceSpec::Kind::St2022) {
            r->st2022 = std::make_unique<St2022Receiver>();
            if (!r->st2022->start(spec.st2022, frames, audio, error)) { std::printf("%s: %s\n", spec.name.c_str(), error.c_str()); return 1; }
            std::printf("%-8s ST 2022-%s %s:%u%s%s\n", spec.name.c_str(), spec.st2022.haveB ? "7" : "6",
                        spec.st2022.a.group.c_str(), spec.st2022.a.port,
                        spec.st2022.haveB ? " + " : "", spec.st2022.haveB ? spec.st2022.b.group.c_str() : "");
        } else {
            r->sdi = std::make_unique<SdiCapture>();
            if (!r->sdi->start(spec.sdi, frames, audio, error)) { std::printf("%s: %s\n", spec.name.c_str(), error.c_str()); return 1; }
            std::printf("%-8s DeckLink %d, mode %s\n", spec.name.c_str(), spec.sdi.device, spec.sdi.mode.c_str());
        }
        running.push_back(std::move(r));
    }
    if (!refName.empty()) std::printf("reference: %s\n\n", refName.c_str());
    else std::printf("one source: lip sync only\n\n");

    const auto t0 = std::chrono::steady_clock::now();
    auto lastReport = t0;
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - t0).count();
        if (seconds > 0.0 && elapsed >= seconds) break;
        if (std::chrono::duration<double>(now - lastReport).count() < interval) continue;
        lastReport = now;

        std::printf("---- %.0f s\n", elapsed);
        for (auto& r : running) {
            std::lock_guard<std::mutex> lk(r->mutex);
            std::string extra;
            if (r->st2022) {
                const St2022Stats s = r->st2022->stats();
                char buf[240];
                std::snprintf(buf, sizeof buf, "  A %llu B %llu datagrams, missing %llu, foreign %llu, bad frames %llu%s%s",
                              static_cast<unsigned long long>(s.datagramsA), static_cast<unsigned long long>(s.datagramsB),
                              static_cast<unsigned long long>(s.missing), static_cast<unsigned long long>(s.foreign),
                              static_cast<unsigned long long>(s.framesBad),
                              s.lastProblem.empty() ? "" : " (", s.lastProblem.empty() ? "" : (s.lastProblem + ")").c_str());
                extra = buf;
            } else if (r->sdi) {
                const SdiStats s = r->sdi->stats();
                if (!s.lastProblem.empty()) extra = "  (" + s.lastProblem + ")";
            }
            const Region reg = r->analyzer->region();
            char regionText[64] = "";
            if (reg.valid())
                std::snprintf(regionText, sizeof regionText, "  picture %d,%d %dx%d%s", reg.x, reg.y, reg.w, reg.h,
                              r->analyzer->regionFromFlash() ? " (from flash)" : "");
            std::printf("%-8s %s  frames %llu  markers %llu  flashes %llu  mutes %llu%s%s\n", r->spec.name.c_str(),
                        r->format.empty() ? "(no frames yet)" : r->format.c_str(),
                        static_cast<unsigned long long>(r->frames), static_cast<unsigned long long>(r->markers),
                        static_cast<unsigned long long>(r->flashes), static_cast<unsigned long long>(r->mutes),
                        regionText, extra.c_str());
        }
        for (const auto& line : correlator.report()) std::printf("%s\n", line.c_str());
        std::fflush(stdout);
    }

    for (auto& r : running) {
        if (r->st2022) r->st2022->stop();
        if (r->sdi) r->sdi->stop();
    }
    for (const auto& line : correlator.report()) std::printf("%s\n", line.c_str());
    if (csv) std::fclose(csv);
    return 0;
}
