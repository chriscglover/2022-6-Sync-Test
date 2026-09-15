// sender -- a moving-ball, burnt-in-timecode ST 2022-6/-7 test signal
// with 1 kHz tone and a flash/mute sync pulse, for measuring delay through a
// receiving system. It only sends; it measures nothing.
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>

#include "pcapreplay/net_interfaces.h"
#include "pcapreplay/net_multicast.h"
#include "pcapreplay/nmos/nmos_node.h"
#include "sdi_output.h"
#include "sender.h"
#include "timecode.h"

using namespace pcapreplay;
using namespace testsignal;

namespace {

constexpr const char* kVersion = "0.1.0";

std::atomic<bool> g_stop{false};

extern "C" void onSignal(int) { g_stop.store(true, std::memory_order_relaxed); }

void installSignalHandlers() {
    struct sigaction sa {};
    sa.sa_handler = onSignal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGHUP, &sa, nullptr);
}

struct FormatAlias {
    const char* name;
    SdiFormat   format;
};

// Field-rate and frame-rate spellings are both accepted for interlaced formats,
// because both are in daily use and neither is wrong.
constexpr FormatAlias kFormatAliases[] = {
    {"1080i50", SdiFormat::HD1080i25},     {"1080i25", SdiFormat::HD1080i25},
    {"1080i59.94", SdiFormat::HD1080i2997}, {"1080i29.97", SdiFormat::HD1080i2997},
    {"1080i60", SdiFormat::HD1080i30},     {"1080i30", SdiFormat::HD1080i30},
    {"1080p25", SdiFormat::HD1080p25},     {"1080p29.97", SdiFormat::HD1080p2997},
    {"1080p30", SdiFormat::HD1080p30},     {"1080p50", SdiFormat::HD1080p50},
    {"1080p59.94", SdiFormat::HD1080p5994}, {"1080p60", SdiFormat::HD1080p60},
    {"1080psf25", SdiFormat::HD1080psf25},
    {"720p50", SdiFormat::HD720p50},       {"720p59.94", SdiFormat::HD720p5994},
    {"720p60", SdiFormat::HD720p60},
    {"625i50", SdiFormat::SD625i25},       {"576i50", SdiFormat::SD625i25},
    {"pal", SdiFormat::SD625i25},
    {"525i59.94", SdiFormat::SD525i2997},  {"486i59.94", SdiFormat::SD525i2997},
    {"ntsc", SdiFormat::SD525i2997},
};

SdiFormat parseFormat(std::string text) {
    for (char& c : text) c = char(std::tolower(static_cast<unsigned char>(c)));
    for (const auto& a : kFormatAliases)
        if (text == a.name) return a.format;
    return SdiFormat::Unknown;
}

int listFormats() {
    std::printf("%-12s %s\n", "--format", "raster");
    for (const auto& a : kFormatAliases)
        std::printf("%-12s %s\n", a.name, formatDescription(a.format).c_str());
    return 0;
}

int listInterfaces() {
    std::printf("%-12s %-16s %-10s %-19s %s\n", "NAME", "ADDRESS", "SPEED", "MAC", "NOTES");
    for (const auto& ni : enumerateInterfaces(true)) {
        std::string speed = "-";
        if (ni.speedBps >= 1000000000ull) speed = std::to_string(ni.speedBps / 1000000000ull) + " Gb/s";
        else if (ni.speedBps >= 1000000ull) speed = std::to_string(ni.speedBps / 1000000ull) + " Mb/s";
        std::string notes;
        if (!ni.up) notes += "down ";
        if (ni.loopback) notes += "loopback ";
        if (ni.virtualAdapter) notes += "virtual ";
        if (!ni.multicastCapable) notes += "no-multicast ";
        if (notes.empty()) notes = "usable";
        std::printf("%-12s %-16s %-10s %-19s %s\n", ni.name.c_str(), ni.ipv4.c_str(), speed.c_str(),
                    ni.mac.empty() ? "-" : ni.mac.c_str(), notes.c_str());
    }
    std::printf("\nPass either the name or the address to --iface.\n");
    return 0;
}

std::string resolveIface(const std::string& given, std::string& error) {
    if (given.empty()) return {};
    NetInterface ni;
    if (findInterfaceByIp(given, ni)) return ni.ipv4;
    if (findInterfaceByName(given, ni)) {
        if (ni.ipv4.empty()) error = "interface '" + given + "' has no IPv4 address";
        return ni.ipv4;
    }
    error = "no interface called '" + given + "' -- run --interfaces to see them";
    return {};
}

void usage() {
    std::printf(
"sender %s - ST 2022-6/-7 delay and A/V sync test signal\n"
"\n"
"usage: sender [options]\n"
"       sender --formats | --interfaces\n"
"\n"
"Picture: a moving ball with the title, burnt-in timecode and frame number, and\n"
"a machine-readable frame marker in the bottom-left corner. Every --flash-every seconds\n"
"the whole picture flashes white for --flash-frames frames and the tone is muted\n"
"for exactly those frames.\n"
"\n"
"Signal\n"
"  --format F             raster (default 1080i50); --formats lists them\n"
"  --title TEXT           top line of the picture (default \"VIDEO TEST SIGNAL\")\n"
"  --flash-every S        seconds between flashes, whole frames (default 2; 0 = none)\n"
"  --flash-frames N       frames each flash and mute lasts (default 1)\n"
"  --tone HZ              tone frequency (default 1000)\n"
"  --tone-level DBFS      tone level on channel 1 (default -18)\n"
"  --tone-step DB         each later channel this much lower (default 3:\n"
"                         16 channels step from -18 to -63 dBFS)\n"
"  --audio-groups N       embedded audio groups, four channels each (default 4)\n"
"  --tc-start TC          timecode on the first frame, HH:MM:SS:FF (default:\n"
"                         time of day when the first frame leaves)\n"
"  --run-tag N            frame marker run tag (default: random, printed at start)\n"
"\n"
"Transmit\n"
"  --group A              path A destination        (default 239.1.1.1)\n"
"  --group-b B            path B destination; giving it enables ST 2022-7\n"
"  --port N               destination port          (default 40000)\n"
"  --iface IF             outgoing interface, by name or address\n"
"  --iface-b IF           a different interface for path B\n"
"  --ttl N                multicast TTL             (default 8)\n"
"  --no-loopback          do not deliver to receivers on this machine\n"
"  --seconds N            stop after N seconds      (default: run until killed)\n"
"\n"
"SDI output\n"
"  --sdi N                send to Blackmagic DeckLink device N instead of the\n"
"                         network, with the same picture, tone and sync pulse.\n"
"                         Needs Blackmagic Desktop Video and the GStreamer\n"
"                         decklink plugin (see README); not with --nmos\n"
"\n"
"NMOS\n"
"  --nmos                 register as an IS-04 sender and serve IS-05\n"
"  --nmos-port N          node API port             (default 3210)\n"
"  --nmos-iface IF        interface the Node API binds and publishes (default --iface)\n"
"  --label TEXT           sender label              (default \"ST 2022 test signal\")\n"
"  --registry H:P         registry override, skips mDNS discovery\n"
"  --no-p2p               do not advertise _nmos-node._tcp\n"
"  --idle                 register, but do not transmit until a controller activates\n"
"\n"
"Other\n"
"  --interfaces           list this machine's NICs and exit\n"
"  --formats              list the rasters and exit\n"
"  --interval N           seconds between console reports (default 2, 0 = off)\n"
"  -h, --help             this\n",
        kVersion);
}

}  // namespace

int main(int argc, char** argv) {
    SenderConfig cfg;
    std::string formatArg = "1080i50", groupB, ifaceArg, ifaceBArg, nmosIfaceArg;
    std::string label = "ST 2022 test signal", registry, tcStart;
    double flashEvery = 2.0, reportInterval = 2.0;
    int port = 40000, nmosPort = 3210, sdiDevice = -1;
    bool wantNmos = false, peerToPeer = true, idle = false, haveRunTag = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if      (a == "-h" || a == "--help") { usage(); return 0; }
        else if (a == "--formats")      return listFormats();
        else if (a == "--interfaces")   return listInterfaces();
        else if (a == "--format")       formatArg = val();
        else if (a == "--title")        cfg.composer.title = val();
        else if (a == "--flash-every")  flashEvery = std::atof(val().c_str());
        else if (a == "--flash-frames") cfg.composer.flashFrames = std::atoi(val().c_str());
        else if (a == "--tone")         cfg.composer.audio.toneHz = std::atof(val().c_str());
        else if (a == "--tone-level")   cfg.composer.audio.levelDbfs = std::atof(val().c_str());
        else if (a == "--tone-step")    cfg.composer.audio.stepDb = std::atof(val().c_str());
        else if (a == "--audio-groups") cfg.composer.audio.groups = std::atoi(val().c_str());
        else if (a == "--tc-start")     tcStart = val();
        else if (a == "--run-tag")      { cfg.composer.runTag = std::strtoull(val().c_str(), nullptr, 0); haveRunTag = true; }
        else if (a == "--group")        cfg.pathA.group = val();
        else if (a == "--group-b")      groupB = val();
        else if (a == "--port")         port = std::atoi(val().c_str());
        else if (a == "--iface")        ifaceArg = val();
        else if (a == "--iface-b")      ifaceBArg = val();
        else if (a == "--ttl")          cfg.ttl = std::atoi(val().c_str());
        else if (a == "--no-loopback")  cfg.loopback = false;
        else if (a == "--seconds")      cfg.maxSeconds = std::atof(val().c_str());
        else if (a == "--sdi")          sdiDevice = std::atoi(val().c_str());
        else if (a == "--nmos")         wantNmos = true;
        else if (a == "--nmos-port")    nmosPort = std::atoi(val().c_str());
        else if (a == "--nmos-iface")   nmosIfaceArg = val();
        else if (a == "--label")        label = val();
        else if (a == "--registry")     registry = val();
        else if (a == "--no-p2p")       peerToPeer = false;
        else if (a == "--idle")         idle = true;
        else if (a == "--interval")     reportInterval = std::atof(val().c_str());
        else {
            std::printf("unknown option %s   (--help for the list)\n", a.c_str());
            return 2;
        }
    }

    installSignalHandlers();

    cfg.composer.format = parseFormat(formatArg);
    if (cfg.composer.format == SdiFormat::Unknown) {
        std::printf("unknown format '%s'   (--formats for the list)\n", formatArg.c_str());
        return 2;
    }
    const SdiFormatInfo& fi = formatInfo(cfg.composer.format);

    if (flashEvery < 0.0 || cfg.composer.flashFrames < 1) {
        std::printf("--flash-every must be 0 or more and --flash-frames at least 1\n");
        return 2;
    }
    cfg.composer.flashPeriodFrames = int(flashEvery * fi.frameRate() + 0.5);
    if (cfg.composer.flashPeriodFrames > 0 &&
        cfg.composer.flashFrames >= cfg.composer.flashPeriodFrames) {
        std::printf("--flash-frames must be shorter than the flash period (%d frames)\n",
                    cfg.composer.flashPeriodFrames);
        return 2;
    }
    if (cfg.composer.audio.groups < 1 || cfg.composer.audio.groups > 4) {
        std::printf("--audio-groups must be 1 to 4\n");
        return 2;
    }
    if (cfg.composer.audio.toneHz <= 0.0 || cfg.composer.audio.toneHz >= 24000.0 ||
        cfg.composer.audio.levelDbfs > 0.0) {
        std::printf("--tone must be below 24000 Hz and --tone-level at most 0 dBFS\n");
        return 2;
    }
    if (cfg.composer.audio.stepDb < 0.0 || cfg.composer.audio.stepDb > 6.0) {
        std::printf("--tone-step must be between 0 and 6 dB\n");
        return 2;
    }
    if (!tcStart.empty()) {
        Timecode tc;
        if (!parseTimecode(tcStart, tc)) {
            std::printf("--tc-start wants HH:MM:SS:FF\n");
            return 2;
        }
        cfg.timecodeFromTimeOfDay = false;
        cfg.composer.timecodeStartFrames =
            framesFromTimecode(tc, timecodeRate(fi.frameRateNum, fi.frameRateDen));
    }
    if (!haveRunTag) {
        std::random_device rd;
        cfg.composer.runTag = (std::uint64_t(rd()) << 32) ^ rd();
    }

    // ---- DeckLink SDI output ------------------------------------------------
    if (sdiDevice >= 0) {
        if (wantNmos) {
            std::printf("--sdi and --nmos cannot be combined: NMOS describes a network sender\n");
            return 2;
        }
        SdiOutputConfig sdi;
        sdi.composer = cfg.composer;
        sdi.timecodeFromTimeOfDay = cfg.timecodeFromTimeOfDay;
        sdi.device = sdiDevice;
        sdi.maxSeconds = cfg.maxSeconds;

        std::printf("sender %s\n", kVersion);
        std::printf("format     : %s\n", formatDescription(fi.id).c_str());
        std::printf("audio      : %d group(s) of tone, %.0f Hz, channel 1 at %.1f dBFS, %g dB lower per channel\n",
                    cfg.composer.audio.groups, cfg.composer.audio.toneHz, cfg.composer.audio.levelDbfs,
                    cfg.composer.audio.stepDb);
        if (cfg.composer.flashPeriodFrames > 0)
            std::printf("sync pulse : white flash + mute for %d frame(s) every %d frames\n",
                        cfg.composer.flashFrames, cfg.composer.flashPeriodFrames);

        SdiOutput output;
        std::string error;
        if (!output.start(sdi, error)) {
            std::printf("error: %s\n", error.c_str());
            return 1;
        }
        const SdiOutputStatus first = output.status();
        std::printf("output     : DeckLink %d, mode %s, %d audio channels\n\n", sdiDevice, first.mode.c_str(),
                    first.audioChannels);

        const auto t0 = std::chrono::steady_clock::now();
        double lastReport = -1.0;
        int exitCode = 0;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            if (g_stop.load(std::memory_order_relaxed)) { std::printf("\nstopping\n"); break; }
            const SdiOutputStatus s = output.status();
            if (!s.error.empty()) { std::printf("error: %s\n", s.error.c_str()); exitCode = 1; break; }
            if (s.completed) break;
            const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (reportInterval > 0.0 && elapsed - lastReport >= reportInterval) {
                lastReport = elapsed;
                std::printf("%8.1fs  %s  frame %llu\n", elapsed, s.timecode.c_str(),
                            static_cast<unsigned long long>(s.framesSent));
                std::fflush(stdout);
            }
        }
        output.stop();
        return exitCode;
    }

    std::string ifaceError;
    const std::string iface = resolveIface(ifaceArg, ifaceError);
    if (!ifaceError.empty()) { std::printf("%s\n", ifaceError.c_str()); return 2; }
    const std::string ifaceB = ifaceBArg.empty() ? iface : resolveIface(ifaceBArg, ifaceError);
    if (!ifaceError.empty()) { std::printf("%s\n", ifaceError.c_str()); return 2; }
    const std::string nmosIfaceGiven = resolveIface(nmosIfaceArg, ifaceError);
    if (!ifaceError.empty()) { std::printf("--nmos-iface: %s\n", ifaceError.c_str()); return 2; }
    const std::string nmosIface = nmosIfaceArg.empty() ? iface : nmosIfaceGiven;

    cfg.pathA.port = std::uint16_t(port);
    cfg.pathA.interfaceIp = iface;
    cfg.enablePathB = !groupB.empty();
    cfg.pathB.group = groupB;
    cfg.pathB.port = std::uint16_t(port);
    cfg.pathB.interfaceIp = ifaceB;

    if (!isValidMulticastGroup(cfg.pathA.group) ||
        (cfg.enablePathB && !isValidMulticastGroup(cfg.pathB.group))) {
        std::printf("destinations must be usable multicast groups (224.0.0.0/4, not 224.0.0.0/24)\n");
        return 2;
    }
    if (cfg.enablePathB && cfg.pathA.group == cfg.pathB.group) {
        std::printf("both ST 2022-7 legs would point at %s, which is not redundancy\n",
                    cfg.pathA.group.c_str());
        return 2;
    }

    WinsockScope sockets;
    std::printf("sender %s\n", kVersion);
    std::printf("format     : %s\n", formatDescription(fi.id).c_str());
    std::printf("audio      : %d group(s), %.0f Hz, channel 1 at %.1f dBFS, %g dB lower per channel\n",
                cfg.composer.audio.groups, cfg.composer.audio.toneHz, cfg.composer.audio.levelDbfs,
                cfg.composer.audio.stepDb);
    if (cfg.composer.flashPeriodFrames > 0)
        std::printf("sync pulse : white flash + mute for %d frame(s) every %d frames\n",
                    cfg.composer.flashFrames, cfg.composer.flashPeriodFrames);
    std::printf("run tag    : %llu (0x%016llx)\n",
                static_cast<unsigned long long>(cfg.composer.runTag),
                static_cast<unsigned long long>(cfg.composer.runTag));

    std::mutex controlMutex;
    std::unique_ptr<nmos::NmosBackend> node(nmos::createBuiltinBackend());
    Sender sender;

    auto query = [&]() {
        std::lock_guard<std::mutex> lk(controlMutex);
        nmos::SenderTransport t;
        const bool live = sender.running();
        const SenderConfig c = live ? sender.activeConfig() : cfg;
        t.active = live;
        t.redundant = c.enablePathB;
        t.sourceIpA = c.pathA.interfaceIp;
        t.sourceIpB = c.pathB.interfaceIp;
        t.destIpA = c.pathA.group;
        t.destPortA = c.pathA.port;
        t.destIpB = c.pathB.group;
        t.destPortB = c.pathB.port;
        t.ttl = c.ttl;
        t.ssrc = c.ssrc;
        t.interfaceNameA = c.pathA.interfaceIp;
        t.interfaceNameB = c.pathB.interfaceIp;
        for (const auto& ni : enumerateInterfaces()) {
            if (ni.ipv4 == c.pathA.interfaceIp) { t.macA = ni.mac; t.interfaceNameA = ni.name; }
            if (ni.ipv4 == c.pathB.interfaceIp) { t.macB = ni.mac; t.interfaceNameB = ni.name; }
        }
        // IS-04 needs a port_id (a MAC) on every listed interface; fall back to
        // the Node API's own interface when the media one has none.
        if (t.macA.empty() && !nmosIface.empty())
            for (const auto& ni : enumerateInterfaces())
                if (ni.ipv4 == nmosIface && !ni.mac.empty()) {
                    t.macA = ni.mac;
                    t.interfaceNameA = ni.name;
                    break;
                }
        if (t.interfaceNameA.empty()) t.interfaceNameA = "any";
        if (t.interfaceNameB.empty()) t.interfaceNameB = "any";
        t.frameRateNum = fi.frameRateNum;
        t.frameRateDen = fi.frameRateDen;
        t.formatText = formatDescription(fi.id);
        return t;
    };

    if (wantNmos) {
        nmos::NmosConfig n;
        n.enabled = true;
        n.label = label;
        n.description = "ST 2022-6/-7 delay and A/V sync test signal";
        n.nodeIp = nmosIface;
        n.nodePort = std::uint16_t(nmosPort);
        n.advertisePeerToPeer = peerToPeer;
        if (!registry.empty()) {
            const std::size_t colon = registry.rfind(':');
            n.useMdns = false;
            n.registryHost = registry.substr(0, colon);
            n.registryPort = std::uint16_t(colon == std::string::npos
                                               ? 3210 : std::atoi(registry.c_str() + colon + 1));
        }
        node->setCallbacks(query, [&](const nmos::SenderTransport& want, std::string& err) {
            {
                std::lock_guard<std::mutex> lk(controlMutex);
                cfg.pathA.group = want.destIpA;
                cfg.pathA.port = want.destPortA;
                if (cfg.enablePathB) {
                    cfg.pathB.group = want.destIpB;
                    cfg.pathB.port = want.destPortB;
                }
                if (!want.active) {
                    sender.stop();
                    std::printf("[nmos] IS-05: sender disabled\n");
                    std::fflush(stdout);
                    return true;
                }
                if (!isValidMulticastGroup(cfg.pathA.group) ||
                    (cfg.enablePathB && !isValidMulticastGroup(cfg.pathB.group))) {
                    err = "destination is not a usable multicast group";
                    return false;
                }
                sender.start(cfg);
            }
            // Confirm the restart rather than assume it: a 200 here tells the
            // controller the sender has moved.
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            for (;;) {
                const SenderStatus s = sender.status();
                if (s.running) break;
                if (!sender.running()) {
                    err = s.error.empty() ? "the sender did not start" : s.error;
                    return false;
                }
                if (std::chrono::steady_clock::now() >= deadline) {
                    err = "the sender did not come up within 10 s";
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            std::printf("[nmos] IS-05: sender enabled -> %s:%u\n", want.destIpA.c_str(),
                        unsigned(want.destPortA));
            std::fflush(stdout);
            return true;
        });
        if (!node->start(n))
            std::printf("nmos failed: %s\n", node->status().error.c_str());
        else
            std::printf("nmos node  : %s\n", node->status().nodeApiUrl.c_str());
    }

    if (idle && wantNmos) {
        std::printf("\nidle: registered, waiting for a controller to activate the sender\n");
    } else {
        std::printf("\nsending to %s:%d%s%s\n", cfg.pathA.group.c_str(), port,
                    cfg.enablePathB ? " and " : "", cfg.enablePathB ? groupB.c_str() : "");
        sender.start(cfg);
        if (wantNmos) node->notifyChanged();
    }

    double lastReport = -1.0;
    bool reportedStart = false;
    int exitCode = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (g_stop.load(std::memory_order_relaxed)) {
            std::printf("\nstopping\n");
            break;
        }
        const SenderStatus s = sender.status();
        if (!s.error.empty() && !sender.running()) {
            std::printf("error: %s\n", s.error.c_str());
            if (!wantNmos) { exitCode = 1; break; }
        }
        if (s.completed && !sender.running() && !wantNmos) break;
        if (s.running && !reportedStart) {
            reportedStart = true;
            std::printf("path A     : %s\n", s.destinationA.c_str());
            if (!s.destinationB.empty()) std::printf("path B     : %s\n", s.destinationB.c_str());
            if (!s.warning.empty()) std::printf("warning    : %s\n", s.warning.c_str());
        }
        if (!s.running) reportedStart = false;
        if (reportInterval > 0.0 && s.running && s.elapsedSeconds - lastReport >= reportInterval) {
            lastReport = s.elapsedSeconds;
            std::printf("%8.1fs  %s  frame %-9llu %7.0f pps (%5.1f%%)  %6.0f Mb/s/leg"
                        "  repeats %llu  late %.2f ms%s\n",
                        s.elapsedSeconds, s.timecode.c_str(),
                        static_cast<unsigned long long>(s.framesSent), s.achievedPps,
                        s.targetPps > 0 ? 100.0 * s.achievedPps / s.targetPps : 0.0, s.wireMbps,
                        static_cast<unsigned long long>(s.repeatedFrames), s.maxLatenessUs / 1000.0,
                        s.audioOverflowPackets ? "  AUDIO OVERFLOW" : "");
            std::fflush(stdout);
        }
    }

    sender.stop();
    if (wantNmos) node->stop();
    return exitCode;
}
