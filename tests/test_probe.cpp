// Self-tests for st2022_delayprobe: marker decoding at native and scaled
// positions, ST 2022-7 merging with loss, duplication and reordering on both
// legs, picture location from a flash, embedded-audio recovery and mute
// timing, and the delay and lip-sync arithmetic.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "composer.h"
#include "overlay.h"
#include "pcapreplay/hbrmt.h"
#include "pcapreplay/sdi_format.h"
#include "probe/analyzer.h"
#include "probe/marker_decode.h"
#include "probe/st2022_receiver.h"

using namespace pcapreplay;
using namespace testsignal;

namespace {

int g_failures = 0, g_checks = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) { ++g_failures; std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

std::vector<std::uint8_t> uyvyBlack(int w, int h) {
    std::vector<std::uint8_t> p(std::size_t(w) * 2 * h);
    for (std::size_t i = 0; i < p.size(); i += 2) { p[i] = 128; p[i + 1] = 16; }
    return p;
}

std::vector<std::uint8_t> lumaOf(const std::vector<std::uint8_t>& uyvy, int w, int h) {
    std::vector<std::uint8_t> y(std::size_t(w) * h);
    for (std::size_t i = 0; i < y.size(); ++i) y[i] = uyvy[2 * i + 1];
    return y;
}

// Nearest-neighbour scale of a luma picture into a rectangle of a black frame.
std::vector<std::uint8_t> placeScaled(const std::vector<std::uint8_t>& src, int sw, int sh,
                                      int fw, int fh, int rx, int ry, int rw, int rh) {
    std::vector<std::uint8_t> out(std::size_t(fw) * fh, 16);
    for (int y = 0; y < rh; ++y)
        for (int x = 0; x < rw; ++x)
            out[std::size_t(ry + y) * fw + std::size_t(rx + x)] =
                src[std::size_t(y * sh / rh) * sw + std::size_t(x * sw / rw)];
    return out;
}

PackedFrame packedOf(std::span<const std::uint8_t> bytes, const SdiFormatInfo& fi, std::int64_t arrivalNs) {
    PackedFrame p;
    p.bytes.assign(bytes.begin(), bytes.end());
    const HbrmtHeader hb = hbrmtForFormat(fi);
    p.frameCode = hb.frame;
    p.frateCode = hb.frate;
    p.sampleCode = hb.sample;
    p.arrivalNs = arrivalNs;
    return p;
}

void testMarker() {
    std::printf("marker decode\n");
    const int w = 1920, h = 1080;
    auto uyvy = uyvyBlack(w, h);
    drawMarker({uyvy.data(), w, h, w * 2}, 0xABCDEF0123456789ull, 123456, 1789486000123ull);
    const auto luma = lumaOf(uyvy, w, h);

    MarkerRead m = decodeMarker(luma.data(), w, h, nativeMarkerBox(w, h));
    CHECK(m.ok && m.frameNumber == 123456 && m.runTag == 0xABCDEF0123456789ull && m.captureUtcMs == 1789486000123ull);

    // Scaled into a monitoring tile's picture area, and into quad tiles.
    const int regions[][4] = {{24, 24, 1536, 864}, {0, 0, 960, 540}, {960, 540, 960, 540}};
    for (const auto& r : regions) {
        const auto placed = placeScaled(luma, w, h, w, h, r[0], r[1], r[2], r[3]);
        m = decodeMarker(placed.data(), w, h, scaledMarkerBox(w, h, r[0], r[1], r[2], r[3]));
        CHECK(m.ok && m.frameNumber == 123456);
    }

    // A corrupted marker must not decode to a wrong frame number.
    auto broken = luma;
    const MarkerBox b = nativeMarkerBox(w, h);
    for (int y = b.y + b.h / 3; y < b.y + b.h * 2 / 3; ++y)
        for (int x = b.x + b.w / 4; x < b.x + b.w * 3 / 4; ++x) broken[std::size_t(y) * w + x] ^= 0xFF;
    CHECK(!decodeMarker(broken.data(), w, h, b).ok);
}

struct Packetised {
    std::vector<std::vector<std::uint8_t>> datagrams;
};

// Packetise composed frames exactly as the sender does.
Packetised packetise(const std::vector<std::vector<std::uint8_t>>& frames, const SdiFormatInfo& fi) {
    Packetised out;
    HbrmtHeader hb = hbrmtForFormat(fi);
    hb.r = 3;
    const std::size_t frameBytes = std::size_t(fi.bytesPerFrame());
    const std::size_t dgPerFrame = (frameBytes + kHbrmtPayloadBytes - 1) / kHbrmtPayloadBytes;
    std::uint16_t seq = 65500;   // wraps within the test
    for (std::size_t k = 0; k < frames.size(); ++k) {
        for (std::size_t d = 0; d < dgPerFrame; ++d) {
            RtpHeader rtp;
            rtp.sequence = seq++;
            rtp.timestamp = std::uint32_t(k * 1080000);
            rtp.marker = (d + 1 == dgPerFrame);
            hb.frCount = std::uint8_t(k);
            std::uint8_t chunk[kHbrmtPayloadBytes] = {};
            const std::size_t within = d * kHbrmtPayloadBytes;
            std::memcpy(chunk, frames[k].data() + within, std::min(kHbrmtPayloadBytes, frameBytes - within));
            std::vector<std::uint8_t> dg(kDatagramBytes);
            buildDatagram(rtp, hb, {chunk, kHbrmtPayloadBytes}, dg.data(), dg.size());
            out.datagrams.push_back(std::move(dg));
        }
    }
    return out;
}

void testMerge() {
    std::printf("ST 2022-7 merge and luma recovery\n");
    const SdiFormat format = SdiFormat::HD1080i25;
    const SdiFormatInfo& fi = formatInfo(format);
    ComposerSettings s;
    s.format = format;
    s.flashPeriodFrames = 0;
    FrameComposer composer(s);

    const int n = 5;
    std::vector<std::vector<std::uint8_t>> frames, pictures;
    for (int k = 0; k < n; ++k) {
        auto pic = uyvyBlack(fi.activeWidth, fi.activeHeight);
        for (int y = 0; y < fi.activeHeight; ++y)          // a different ramp per frame
            for (int x = 0; x < fi.activeWidth; ++x)
                pic[std::size_t(y) * fi.activeWidth * 2 + 2 * x + 1] = std::uint8_t(16 + (x + 3 * y + 17 * k) % 219);
        const auto packed = composer.compose({pic.data(), fi.activeWidth, fi.activeHeight, fi.activeWidth * 2}, k, 0);
        frames.emplace_back(packed.begin(), packed.end());
        pictures.push_back(lumaOf(pic, fi.activeWidth, fi.activeHeight));
    }
    const Packetised p = packetise(frames, fi);

    std::vector<PackedFrame> out;
    St2022Merger merger([&](PackedFrame&& f) { out.push_back(std::move(f)); });

    // Leg A loses every 11th datagram, leg B every 13th except where A already
    // lost it (so every datagram survives on at least one leg), B runs a few
    // datagrams behind A, and a short stretch is delivered out of order.
    std::vector<std::pair<std::size_t, int>> order;   // (datagram, leg)
    const std::size_t total = p.datagrams.size();
    auto lostA = [](std::size_t j) { return j % 11 == 5; };
    auto lostB = [&](std::size_t j) { return j % 13 == 3 && !lostA(j); };
    for (std::size_t i = 0; i < total; ++i) {
        if (!lostA(i)) order.push_back({i, 0});
        if (i >= 7 && !lostB(i - 7)) order.push_back({i - 7, 1});
    }
    for (std::size_t i = total - 7; i < total; ++i)
        if (!lostB(i)) order.push_back({i, 1});
    std::mt19937 rng(7);
    for (std::size_t i = 1000; i + 8 < order.size() && i < 1400; i += 8)
        std::shuffle(order.begin() + std::ptrdiff_t(i), order.begin() + std::ptrdiff_t(i + 8), rng);

    std::int64_t t = 1'000'000;
    for (const auto& [index, leg] : order)
        merger.push(p.datagrams[index].data(), p.datagrams[index].size(), t++ + leg * 1000);

    // Frame 0 is consumed synchronising to a marker; 1..3 must come out whole
    // (4 is held until more data proves no datagram is missing).
    CHECK(out.size() >= 3);
    CHECK(merger.missing() == 0);
    CHECK(merger.duplicates() > 0);
    for (std::size_t i = 0; i < out.size(); ++i) {
        LumaFrame luma;
        std::string problem;
        const bool ok = extractLuma(out[i], luma, problem);
        CHECK(ok);
        if (!ok) { std::printf("    frame %zu: %s\n", i, problem.c_str()); continue; }
        const auto& want = pictures[i + 1];
        CHECK(luma.width == fi.activeWidth && luma.height == fi.activeHeight);
        int mismatches = 0;
        for (std::size_t k = 0; k < want.size(); k += 101)
            if (luma.luma[k] != want[k]) ++mismatches;
        CHECK(mismatches == 0);
    }

    // A frame with a hole neither leg filled is reported, not passed on. The
    // hole is in frame 1: frame 0 is consumed synchronising to its marker.
    std::vector<PackedFrame> holed;
    St2022Merger lossy([&](PackedFrame&& f) { holed.push_back(std::move(f)); }, 1024);
    for (std::size_t i = 0; i < total; ++i)
        if (i != 8000) lossy.push(p.datagrams[i].data(), p.datagrams[i].size(), std::int64_t(i));
    CHECK(lossy.missing() == 1);
    int incomplete = 0;
    for (const auto& f : holed) if (!f.complete) ++incomplete;
    CHECK(incomplete == 1);

    // A second stream on the same socket (another SSRC) never enters the merge.
    St2022Merger strict([&](PackedFrame&&) {});
    strict.push(p.datagrams[0].data(), p.datagrams[0].size(), 0);
    auto other = p.datagrams[1];
    other[8] ^= 0x55;   // RTP SSRC
    CHECK(!strict.push(other.data(), other.size(), 1));
    CHECK(strict.foreign() == 1);
}

void testAudio(SdiFormat format) {
    const SdiFormatInfo& fi = formatInfo(format);
    std::printf("audio recovery and mute timing, %s\n", fi.name);
    ComposerSettings s;
    s.format = format;
    s.flashPeriodFrames = 4;   // frames 0, 4, 8 flash and mute
    FrameComposer composer(s);

    const double frameNs = 1e9 * double(fi.frameRateDen) / double(fi.frameRateNum);
    MuteDetector detector;
    std::vector<std::int64_t> mutes;
    const std::int64_t base = 5'000'000'000;
    for (int k = 0; k < 10; ++k) {
        auto pic = uyvyBlack(fi.activeWidth, fi.activeHeight);
        const auto bytes = composer.compose({pic.data(), fi.activeWidth, fi.activeHeight, fi.activeWidth * 2}, k, 0);
        // Arrival at the frame's first audio sample time, so timing is exact.
        const std::uint64_t firstSample = AudioEmbedder::firstSampleOfFrame(fi, std::uint64_t(k));
        const std::int64_t arrival = base + std::int64_t(std::llround(double(firstSample) * 1e9 / 48000.0));
        AudioBlock block;
        CHECK(extractAudio(packedOf(bytes, fi, arrival), block));
        const std::uint64_t expected = AudioEmbedder::firstSampleOfFrame(fi, std::uint64_t(k + 1)) - firstSample;
        CHECK(block.samples.size() == expected);
        int wrong = 0;
        const double scale = fi.isHd ? 8388608.0 : 8388608.0;
        for (std::size_t i = 0; i < block.samples.size(); ++i) {
            std::int32_t want = composer.isFlash(std::uint64_t(k)) ? 0 : composer.audio().toneSample(firstSample + i);
            if (!fi.isHd) want = (want >> 4) * 16;
            if (std::fabs(double(block.samples[i]) * scale - double(want)) > 1.0) ++wrong;
        }
        CHECK(wrong == 0);
        detector.feed(block, mutes);
    }
    CHECK(detector.hearingTone());
    CHECK(std::fabs(detector.toneDbfs() - (-18.0)) < 0.2);
    // Mutes at frames 4 and 8 (frame 0 has no tone before it to stop).
    CHECK(mutes.size() == 2);
    for (std::size_t i = 0; i < mutes.size() && i < 2; ++i) {
        const std::uint64_t firstSample = AudioEmbedder::firstSampleOfFrame(fi, 4 * (i + 1));
        const std::int64_t want = base + std::int64_t(std::llround(double(firstSample) * 1e9 / 48000.0));
        // Within one sample (a zero-crossing sample of the tone can sit next to the silence).
        CHECK(std::llabs(mutes[i] - want) <= 21'000);
    }
    (void)frameNs;
}

void testAnalyzer() {
    std::printf("analyzer: flash, picture location, scaled marker\n");
    const int w = 1920, h = 1080;
    const int region[4] = {24, 24, 1536, 864};
    Analyzer analyzer;
    analyzer.setSourceRaster(w, h);

    auto frameFor = [&](int k, bool flash) {
        auto uyvy = uyvyBlack(w, h);
        const UyvyImage img{uyvy.data(), w, h, w * 2};
        if (flash) fillFrame(img, kWhiteY, kNeutralC, kNeutralC);
        drawMarker(img, 99, std::uint32_t(k), 0);
        auto placed = placeScaled(lumaOf(uyvy, w, h), w, h, w, h, region[0], region[1], region[2], region[3]);
        // Furniture outside the picture that does not flash: a bright meter column.
        for (int y = 100; y < 900; ++y)
            for (int x = 1700; x < 1760; ++x) placed[std::size_t(y) * w + x] = 180;
        LumaFrame f;
        f.width = w; f.height = h; f.luma = std::move(placed);
        f.fpsNum = 50; f.fpsDen = 1;
        f.arrivalNs = std::int64_t(k) * 20'000'000;
        return f;
    };

    for (int k = 0; k < 10; ++k) CHECK(!analyzer.analyse(frameFor(k, false)).flash);
    const Observation flash = analyzer.analyse(frameFor(10, true));
    CHECK(flash.flash && flash.flashStart);
    const Region r = analyzer.region();
    CHECK(analyzer.regionFromFlash());
    CHECK(std::abs(r.x - region[0]) <= 2 && std::abs(r.y - region[1]) <= 2);
    CHECK(std::abs(r.w - region[2]) <= 3 && std::abs(r.h - region[3]) <= 3);
    const Observation after = analyzer.analyse(frameFor(11, false));
    CHECK(!after.flash);
    CHECK(after.frameNumber == 11 && after.runTag == 99);
}

void testCorrelator() {
    std::printf("correlator: delay and lip sync\n");
    Correlator c("in", nullptr);
    // Reference 1080i25: frame k arrives at 40 ms * k. Output 1080p50 with a
    // 2-frame capture offset: each frame number shows on two output frames,
    // the first at reference + 400 ms + 40 ms (two 20 ms frames).
    for (int k = 0; k < 60; ++k) {
        Observation o;
        o.arrivalNs = std::int64_t(k) * 40'000'000;
        o.frameNumber = k;
        o.runTag = 5;
        o.framePeriodNs = 40e6;
        o.flashStart = (k == 50);
        c.observe("in", o, 0);
    }
    for (int k = 0; k < 50; ++k) {
        for (int rep = 0; rep < 2; ++rep) {
            Observation o;
            o.arrivalNs = std::int64_t(k) * 40'000'000 + 440'000'000 + rep * 20'000'000;
            o.frameNumber = k;
            o.runTag = 5;
            o.framePeriodNs = 20e6;
            c.observe("out", o, 2.0);
        }
    }
    Observation flash;
    flash.arrivalNs = 50 * 40'000'000LL + 440'000'000;
    flash.frameNumber = 50;
    flash.runTag = 5;
    flash.framePeriodNs = 20e6;
    flash.flashStart = true;
    c.observe("out", flash, 2.0);

    // Lip sync: the reference's audio is exact; the output's is 12.5 ms late.
    c.noteAudio("in", -18.0);
    c.observeMute("in", 50 * 40'000'000LL);
    c.noteAudio("out", -18.0);
    c.observeMute("out", flash.arrivalNs + 12'500'000);

    const auto lines = c.report();
    CHECK(lines.size() == 2);
    std::string in, out;
    for (const auto& l : lines) {
        std::printf("    %s\n", l.c_str());
        (l.rfind("in", 0) == 0 ? in : out) = l;
    }
    CHECK(out.find("delay  400.00 ms") != std::string::npos);
    CHECK(out.find("51 frames") != std::string::npos);
    CHECK(out.find("flash 400.0 ms") != std::string::npos);
    CHECK(out.find("lip sync +12.5 ms, audio late") != std::string::npos);
    CHECK(in.find("lip sync +0.0 ms") != std::string::npos);
    CHECK(in.find("delay") == std::string::npos);

    // A single source reports lip sync alone.
    Correlator solo("", nullptr);
    Observation f;
    f.arrivalNs = 1'000'000'000;
    f.flashStart = true;
    f.framePeriodNs = 40e6;
    solo.observe("sender", f, 0);
    solo.observeMute("sender", 1'000'000'000 - 3'000'000);
    const auto soloLines = solo.report();
    CHECK(soloLines.size() == 1 && soloLines[0].find("lip sync -3.0 ms, audio early") != std::string::npos);
}

}  // namespace

int main() {
    testMarker();
    testMerge();
    testAudio(SdiFormat::HD1080i25);
    testAudio(SdiFormat::HD1080i2997);
    testAudio(SdiFormat::SD625i25);
    testAnalyzer();
    testCorrelator();
    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
