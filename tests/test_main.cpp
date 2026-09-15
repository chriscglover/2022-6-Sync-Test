// Self-tests: compose real frames, then take them apart again the way an
// ST 2022-6 receiver does -- frames cut at line 1's EAV, timing references
// flagged per the ST 274 / ST 259 / SMPTE 125M line tables, the ST 292 line
// CRC over the preceding active picture, ST 291 packet integrity, and the
// embedded audio decoded back to samples.
//
// The checks deliberately restate those rules rather than reuse the composer's
// own tables: a decoder checked only against its own writer agrees with itself
// by construction.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "audio_embed.h"
#include "composer.h"
#include "decklink_modes.h"
#include "overlay.h"
#include "pcapreplay/bitpack.h"
#include "pcapreplay/crc.h"
#include "pcapreplay/sdi_raster.h"
#include "timecode.h"

using namespace pcapreplay;
using namespace testsignal;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                        \
    } while (0)

bool nineBitParityOk(std::uint16_t w) {
    return (__builtin_popcount(w & 0x1FF) % 2) == 0 && (((w >> 9) ^ (w >> 8)) & 1) == 1;
}

// The receiving side's F/V tables, restated.
struct Expected {
    bool f, v;
    int  row;   // -1 = no picture
};

Expected receiverRules(const SdiFormatInfo& fi, int line) {
    switch (fi.id) {
        case SdiFormat::HD1080i25: case SdiFormat::HD1080i2997: case SdiFormat::HD1080i30:
        case SdiFormat::HD1080psf25: {
            const bool v = line <= 20 || (line >= 561 && line <= 583) || line >= 1124;
            int row = -1;
            if (line >= 21 && line <= 560) row = (line - 21) * 2;
            else if (line >= 584 && line <= 1123) row = (line - 584) * 2 + 1;
            return {line >= 564, v, row};
        }
        case SdiFormat::SD625i25: {
            const bool v = line <= 22 || (line >= 311 && line <= 335) || line >= 624;
            int row = -1;
            if (line >= 23 && line <= 310) row = (line - 23) * 2;
            else if (line >= 336 && line <= 623) row = (line - 336) * 2 + 1;
            return {line >= 313, v, row};
        }
        case SdiFormat::SD525i2997: {
            const bool v = line <= 19 || (line >= 264 && line <= 282);
            int row = -1;   // bottom field first: field 1 (21..) on odd rows
            if (line >= 21 && line <= 263) row = (line - 21) * 2 + 1;
            else if (line >= 283 && line <= 525) row = (line - 283) * 2;
            return {line <= 3 || line >= 266, v, row};
        }
        default: {
            // Progressive: picture from activeStartF1 for activeHeight lines.
            const bool active = line >= fi.activeStartF1 && line < fi.activeStartF1 + fi.activeHeight;
            return {false, !active, active ? line - fi.activeStartF1 : -1};
        }
    }
}

std::uint16_t xyz(bool f, bool v, bool h) {
    const int p3 = v ^ h, p2 = f ^ h, p1 = f ^ v, p0 = f ^ v ^ h;
    return std::uint16_t(0x200 | (f << 8) | (v << 7) | (h << 6) | (p3 << 5) | (p2 << 4) |
                         (p1 << 3) | (p0 << 2));
}

struct WireCheck {
    int badEav = 0, badSav = 0, badLn = 0, badCrc = 0, badPicture = 0;
    int firstBadLine = 0;
};

WireCheck checkWire(const SdiFormatInfo& fi, const std::vector<std::uint16_t>& w,
                    const std::vector<std::uint8_t>& picture) {
    WireCheck r;
    const int lw = fi.totalSamples * 2;
    const int aw = fi.activeWidth * 2;
    auto bad = [&](int& counter, int line) { if (!counter++ && !r.firstBadLine) r.firstBadLine = line; };
    for (int line = 1; line <= fi.totalLines; ++line) {
        const Expected e = receiverRules(fi, line);
        const std::size_t u = std::size_t(line - 1) * lw;
        if (fi.isHd) {
            const std::uint16_t x = xyz(e.f, e.v, true), s = xyz(e.f, e.v, false);
            if (!(w[u] == 0x3FF && w[u + 1] == 0x3FF && w[u + 2] == 0 && w[u + 3] == 0 &&
                  w[u + 4] == 0 && w[u + 5] == 0 && w[u + 6] == x && w[u + 7] == x)) bad(r.badEav, line);
            const std::size_t sv = u + std::size_t(lw - aw - 8);
            if (!(w[sv] == 0x3FF && w[sv + 1] == 0x3FF && w[sv + 6] == s && w[sv + 7] == s)) bad(r.badSav, line);
            if (lineNumberFromWords(w[u + 8], w[u + 10]) != line) bad(r.badLn, line);
            // ST 292: CRC over the active words before EAV plus EAV and LN.
            if (u >= std::size_t(aw)) {
                const std::uint32_t c = crc18Strided(&w[u - aw], std::size_t(fi.activeWidth) + 6, 2);
                const std::uint32_t y = crc18Strided(&w[u - aw + 1], std::size_t(fi.activeWidth) + 6, 2);
                if (c != crc18FromWords(w[u + 12], w[u + 14]) || y != crc18FromWords(w[u + 13], w[u + 15]))
                    bad(r.badCrc, line);
            }
        } else {
            if (!(w[u] == 0x3FF && w[u + 1] == 0 && w[u + 2] == 0 && w[u + 3] == xyz(e.f, e.v, true)))
                bad(r.badEav, line);
            const std::size_t sv = u + std::size_t(lw - aw - 4);
            if (!(w[sv] == 0x3FF && w[sv + 1] == 0 && w[sv + 2] == 0 && w[sv + 3] == xyz(e.f, e.v, false)))
                bad(r.badSav, line);
        }
        // Picture: the active words at the tail of the line are the expected row.
        const std::size_t a = u + std::size_t(lw - aw);
        if (e.row >= 0) {
            const std::uint8_t* src = picture.data() + std::size_t(e.row) * std::size_t(fi.activeWidth) * 2;
            for (int k = 0; k < aw; k += 97)
                if (w[a + k] != std::uint16_t(src[k]) << 2) { bad(r.badPicture, line); break; }
        } else if (w[a] != kBlankChroma || w[a + 1] != kBlankLuma) {
            bad(r.badPicture, line);
        }
    }
    return r;
}

struct DecodedAudio {
    std::map<int, std::vector<std::int32_t>> samples;   // channel -> samples
    int packets = 0;
    int badChecksums = 0;
    int badParity = 0;
    int zFlags = 0;
    bool channelStatusStartsBlock = false;
};

// Walk every wire line's HANC -- between the post-EAV words and the SAV.
DecodedAudio decodeAudio(const SdiFormatInfo& fi, const std::vector<std::uint16_t>& w) {
    const int lineWords = fi.totalSamples * 2;
    DecodedAudio out;
    const int stride = fi.isHd ? 2 : 1;
    const int hancStart = postActiveWords(fi);
    const int hancEnd = lineWords - fi.activeWidth * 2 - savWords(fi);
    for (int ln = 0; ln < fi.totalLines; ++ln) {
        const std::uint16_t* l = w.data() + std::size_t(ln) * lineWords;
        for (int k = hancStart; k + 6 * stride < hancEnd; k += stride) {
            if (l[k] != 0x000 || l[k + stride] != 0x3FF || l[k + 2 * stride] != 0x3FF) continue;
            const int did = l[k + 3 * stride] & 0xFF;
            const int dc = l[k + 5 * stride] & 0xFF;
            std::uint16_t words[300];
            for (int q = 0; q < 3 + dc; ++q) words[q] = l[k + (3 + q) * stride];
            const std::uint16_t sum = l[k + (6 + dc) * stride];
            ++out.packets;
            if (ancChecksum(words, std::size_t(3 + dc)) != sum) ++out.badChecksums;
            // DID, DBN and DC are 8-bit words with even parity in b8. HD audio
            // UDWs are too; ST 272 sample words carry audio in b8 and are
            // checked per sample below, so for them only b9 = !b8 applies.
            for (int q = 0; q < 3 + dc; ++q) {
                const bool eightBit = fi.isHd || q < 3;
                if (eightBit ? !nineBitParityOk(words[q]) : ((((words[q] >> 9) ^ (words[q] >> 8)) & 1) != 1))
                    ++out.badParity;
            }
            const std::uint16_t* udw = words + 3;
            if (fi.isHd) {
                const int group = 0xE7 - did;
                for (int ch = 0; ch < 4; ++ch) {
                    std::uint32_t sub = 0;
                    for (int b = 0; b < 4; ++b) sub |= std::uint32_t(udw[2 + ch * 4 + b] & 0xFF) << (8 * b);
                    if (__builtin_popcount(sub >> 4) % 2) ++out.badParity;
                    if (ch % 2 == 0 && (udw[2 + ch * 4] & 0x08)) {
                        ++out.zFlags;
                        if (sub & (1u << 30)) out.channelStatusStartsBlock = true;
                    }
                    out.samples[group * 4 + ch].push_back(std::int32_t(sub << 4) >> 8);
                }
            } else {
                const int group = (0xFF - did) / 2;
                for (int x = 0; x < dc; x += 3) {
                    const std::uint32_t w0 = udw[x], w1 = udw[x + 1], w2 = udw[x + 2];
                    const int ch = int((w0 >> 1) & 3);
                    if ((__builtin_popcount(w0 & 0x1FF) + __builtin_popcount(w1 & 0x1FF) +
                         __builtin_popcount(w2 & 0x1FF)) % 2)
                        ++out.badParity;
                    if (w0 & 1) ++out.zFlags;
                    std::uint32_t aud = ((w0 >> 3) & 0x3F) | ((w1 & 0x1FF) << 6) | ((w2 & 0x1F) << 15);
                    std::int32_t s = std::int32_t(aud);
                    if (s & 0x80000) s -= 0x100000;
                    out.samples[group * 4 + ch].push_back(s * 16);
                }
            }
            k += (6 + dc) * stride;
        }
    }
    return out;
}

// A picture whose rows are all different, so a row swap cannot pass.
std::vector<std::uint8_t> rampPicture(const SdiFormatInfo& fi) {
    std::vector<std::uint8_t> pic(std::size_t(fi.activeWidth) * 2 * fi.activeHeight);
    for (int y = 0; y < fi.activeHeight; ++y)
        for (int x = 0; x < fi.activeWidth * 2; ++x)
            pic[std::size_t(y) * fi.activeWidth * 2 + x] =
                (x & 1) ? std::uint8_t(16 + (y * 7 + x) % 219) : std::uint8_t(64 + (y * 3) % 128);
    return pic;
}

void testTimecode() {
    std::printf("timecode\n");
    const TimecodeRate df = timecodeRate(30000, 1001);
    CHECK(df.fps == 30 && df.dropFrame);
    CHECK(timecodeText(timecodeFromFrames(1799, df)) == "00:00:59;29");
    CHECK(timecodeText(timecodeFromFrames(1800, df)) == "00:01:00;02");
    CHECK(timecodeText(timecodeFromFrames(17982, df)) == "00:10:00;00");
    Timecode tc;
    CHECK(parseTimecode("10:00:00;00", tc));
    CHECK(timecodeText(timecodeFromFrames(framesFromTimecode(tc, df), df)) == "10:00:00;00");
    const TimecodeRate pal = timecodeRate(25, 1);
    CHECK(pal.fps == 25 && !pal.dropFrame);
    CHECK(timecodeText(timecodeFromFrames(25 * 3600 + 7, pal)) == "01:00:00:07");
    CHECK(timecodeText(timecodeFromFrames(framesPerDay(pal), pal)) == "00:00:00:00");
}

void testMarkerPayload() {
    std::printf("frame marker payload\n");
    // Reference vector for the 24-byte payload layout.
    const std::uint8_t expected[24] = {0x4d, 0x56, 0x01, 0x00, 0x01, 0x02, 0x03, 0x04,
                                       0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c,
                                       0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0xf9, 0xcf};
    const auto p = markerPayload(0x0102030405060708ull, 0x090a0b0c, 0x0d0e0f101112ull);
    CHECK(std::memcmp(p.data(), expected, 24) == 0);
}

void testDeckLinkModes() {
    std::printf("DeckLink modes\n");
    const DeckLinkMode* m = deckLinkModeFor(SdiFormat::HD1080i25);
    CHECK(m && std::string(m->nick) == "1080i50" && m->fieldOrder && std::string(m->fieldOrder) == "top-field-first");
    m = deckLinkModeFor(SdiFormat::HD1080p5994);
    CHECK(m && std::string(m->nick) == "1080p5994" && m->fieldOrder == nullptr);
    m = deckLinkModeFor(SdiFormat::SD525i2997);
    CHECK(m && std::string(m->nick) == "ntsc" && std::string(m->fieldOrder) == "bottom-field-first" &&
          std::string(m->pixelAspect) == "10/11");
    CHECK(deckLinkModeFor(SdiFormat::HD1080psf25) == nullptr);
    // Every other format the sender offers has a mode.
    int missing = 0;
    for (const auto& fi : allFormats())
        if (fi.id != SdiFormat::HD1080psf25 && !deckLinkModeFor(fi.id)) ++missing;
    CHECK(missing == 0);
    CHECK(deckLinkAudioChannels(1) == 8 && deckLinkAudioChannels(3) == 16);
}

void testChannelStatus() {
    std::printf("channel status\n");
    const auto cs = professionalChannelStatus(24);
    CHECK((cs[0] & 1) == 1);                // professional
    CHECK((cs[0] >> 6) == 2);               // 48 kHz
    CHECK(channelStatusCrc(cs.data()) == cs[23]);
}

void testFormat(SdiFormat format, int flashPeriod, int groups) {
    const SdiFormatInfo& fi = formatInfo(format);
    std::printf("%s, %d group(s)\n", fi.name, groups);

    ComposerSettings s;
    s.format = format;
    s.flashPeriodFrames = flashPeriod;
    s.flashFrames = 1;
    s.audio.groups = groups;
    s.runTag = 42;
    FrameComposer composer(s);

    const int nFrames = 6;
    std::uint64_t totalSamples = 0;
    for (int k = 0; k < nFrames; ++k) {
        auto pic = rampPicture(fi);
        UyvyImage img{pic.data(), fi.activeWidth, fi.activeHeight, fi.activeWidth * 2};
        const auto packed = composer.compose(img, std::uint64_t(k), 1000);
        CHECK(std::int64_t(packed.size()) == fi.bytesPerFrame());

        std::vector<std::uint16_t> w(std::size_t(fi.totalSamples) * 2 * fi.totalLines);
        unpack10(packed.data(), w.size(), w.data());

        const WireCheck wc = checkWire(fi, w, pic);
        CHECK(wc.badEav == 0);
        CHECK(wc.badSav == 0);
        CHECK(wc.badLn == 0);
        CHECK(wc.badCrc == 0);
        CHECK(wc.badPicture == 0);
        if (wc.firstBadLine)
            std::printf("    frame %d: first bad line %d (eav %d sav %d ln %d crc %d picture %d)\n", k,
                        wc.firstBadLine, wc.badEav, wc.badSav, wc.badLn, wc.badCrc, wc.badPicture);

        const DecodedAudio audio = decodeAudio(fi, w);
        const std::uint64_t expect = AudioEmbedder::firstSampleOfFrame(fi, k + 1) -
                                     AudioEmbedder::firstSampleOfFrame(fi, k);
        totalSamples += expect;
        CHECK(audio.badChecksums == 0);
        CHECK(audio.badParity == 0);
        CHECK(int(audio.samples.size()) == 4 * groups);
        const auto& ch1 = audio.samples.count(0) ? audio.samples.at(0) : std::vector<std::int32_t>{};
        CHECK(ch1.size() == expect);
        CHECK(composer.audio().stats().overflowedPackets == 0);

        // Samples decode to the tone, or to silence on the flash frame.
        const std::uint64_t first = AudioEmbedder::firstSampleOfFrame(fi, k);
        int mismatches = 0;
        for (std::size_t i = 0; i < ch1.size(); ++i) {
            std::int32_t want = composer.isFlash(std::uint64_t(k)) ? 0 : composer.audio().toneSample(first + i);
            if (!fi.isHd) want = (want >> 4) * 16;
            if (ch1[i] != want) ++mismatches;
        }
        CHECK(mismatches == 0);

        // Every channel carries the tone, each at its own step of the staircase.
        for (int channel = 1; channel < 4 * groups; ++channel) {
            const auto it = audio.samples.find(channel);
            if (it == audio.samples.end()) { CHECK(false); continue; }
            int wrong = it->second.size() == ch1.size() ? 0 : 1;
            for (std::size_t i = 0; i < it->second.size() && i < ch1.size(); ++i) {
                std::int32_t want = composer.isFlash(std::uint64_t(k)) ? 0 : composer.audio().toneSample(first + i, channel);
                if (!fi.isHd) want = (want >> 4) * 16;
                if (it->second[i] != want) ++wrong;
            }
            CHECK(wrong == 0);
        }
        if (!composer.isFlash(std::uint64_t(k))) {
            double energy1 = 0;
            for (const std::int32_t v : ch1) energy1 += double(v) * v;
            for (int channel = 1; channel < 4 * groups; ++channel) {
                if (!audio.samples.count(channel)) continue;
                double energy = 0;
                for (const std::int32_t v : audio.samples.at(channel)) energy += double(v) * v;
                const double relativeDb = 10.0 * std::log10(energy / energy1);
                CHECK(std::fabs(relativeDb + s.audio.stepDb * channel) < 0.1);
            }
        }
        if (k == 0) {
            CHECK(audio.zFlags > 0);
            if (fi.isHd) CHECK(audio.channelStatusStartsBlock);
        }
    }
    if (fi.frameRateDen == 1001 && fi.frameRateNum == 30000) {
        CHECK(AudioEmbedder::firstSampleOfFrame(fi, 5) == 8008);
    }
    CHECK(totalSamples == AudioEmbedder::firstSampleOfFrame(fi, nFrames));
}

}  // namespace

int main() {
    testTimecode();
    testMarkerPayload();
    testChannelStatus();
    testDeckLinkModes();
    // Four groups (the default, 16 channels) must fit every raster's HANC.
    testFormat(SdiFormat::HD1080i25, 3, 1);
    testFormat(SdiFormat::HD1080i25, 3, 4);
    testFormat(SdiFormat::HD1080p50, 4, 4);
    testFormat(SdiFormat::HD1080i2997, 3, 4);
    testFormat(SdiFormat::HD1080p5994, 3, 4);
    testFormat(SdiFormat::HD720p5994, 3, 4);
    testFormat(SdiFormat::SD625i25, 3, 4);
    testFormat(SdiFormat::SD525i2997, 3, 4);
    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
