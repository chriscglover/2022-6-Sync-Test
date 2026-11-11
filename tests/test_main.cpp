// Self-tests: compose real frames, then take them apart again the way an
// ST 2022-6 receiver does -- frames cut at line 1's EAV, timing references
// flagged per the ST 274 / ST 259 / SMPTE 125M line tables, the ST 292 line
// CRC over the preceding active picture, ST 291 packet integrity, and the
// embedded audio decoded back to samples.
//
// The checks deliberately restate those rules rather than reuse the composer's
// own tables: a decoder checked only against its own writer agrees with itself
// by construction.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "audio_embed.h"
#include "composer.h"
#include "newfor.h"
#include "teletext.h"
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

void testBackgroundColour() {
    std::printf("background colour\n");
    std::uint32_t rgb = 1;
    CHECK(parseRgb("808080", rgb) && rgb == 0x808080);
    CHECK(parseRgb("#00a0FF", rgb) && rgb == 0x00A0FF);
    for (const char* bad : {"", "80808", "8080800", "grey", "#12345g", "0x8080"})
        CHECK(!parseRgb(bad, rgb));
    CHECK(videoLuma(0x000000) == 16 && videoLuma(0xFFFFFF) == 235);
    // 50% grey is accepted; the white flash stays more than 80 above it.
    CHECK(videoLuma(0x808080) == 126 && videoLuma(0x808080) <= kMaxBackgroundY);
    CHECK(kWhiteY - kMaxBackgroundY > 80);
    CHECK(videoLuma(0xC0C0C0) > kMaxBackgroundY);
}

void testChannelStatus() {
    std::printf("channel status\n");
    const auto cs = professionalChannelStatus(24);
    CHECK((cs[0] & 1) == 1);                // professional
    CHECK((cs[0] >> 6) == 2);               // 48 kHz
    CHECK(channelStatusCrc(cs.data()) == cs[23]);
}

void testSt299Ecc() {
    std::printf("ST 299-1 ECC\n");
    // Group 1, DBN 157, from a third-party embedder's capture (1080p.pcap):
    // ADF, DID, DBN, DC, then UDW0..17, and the ECC it sent.
    const std::uint8_t first24[24] = {
        0x00, 0xFF, 0xFF, 0xE7, 157, 24,
        0x00, 0x00, 0x00, 0x30, 0x09, 0x00, 0x00, 0xF0, 0x09, 0x00, 0xE0, 0x1F,
        0x00, 0x00, 0xF0, 0xFC, 0xFF, 0x0F};
    const std::array<std::uint8_t, 6> sent = {0xC6, 0x8B, 0x4F, 0x0A, 0xEF, 0x46};
    CHECK(st299Ecc(first24) == sent);
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

// ---- teletext ----------------------------------------------------------------
//
// The rules restated from the receiving side, as real broadcast captures carry
// them: Hamming data bits ride b1, b3, b5 and b7; magazine 8 is sent as 0; the
// OP-47 data blocks start at byte 9.

int hamData(std::uint8_t b) {
    return ((b >> 1) & 1) | (((b >> 3) & 1) << 1) | (((b >> 5) & 1) << 2) | (((b >> 7) & 1) << 3);
}

struct Ttx {
    int magazine = 0, packet = 0, page = -1;
    bool erase = false, subtitle = false;
    std::string text;
    std::vector<std::uint8_t> raw;   // the 42 packet bytes
};

Ttx parseTtx(const std::uint8_t* p) {
    Ttx t;
    const int h0 = hamData(p[0]), h1 = hamData(p[1]);
    t.magazine = (h0 & 7) ? (h0 & 7) : 8;
    t.packet = ((h1 << 1) | (h0 >> 3)) & 0x1F;
    if (t.packet == 0) {
        t.page = (t.magazine << 8) | (hamData(p[3]) << 4) | hamData(p[2]);
        t.erase = hamData(p[5]) & 0x8;
        t.subtitle = hamData(p[7]) & 0x8;
    } else {
        for (int i = 2; i < 42; ++i) {
            const int c = p[i] & 0x7F;
            t.text.push_back(c >= 0x20 && c < 0x7F ? char(c) : ' ');
        }
    }
    t.raw.assign(p, p + 42);
    return t;
}

std::array<std::uint8_t, 40> cellsOf(const std::string& text, std::uint8_t colour = 0x07) {
    // NewforTest's layout: double height, two start-box, colour, then text.
    std::array<std::uint8_t, 40> c;
    c.fill(' ');
    std::size_t i = 0;
    c[i++] = 0x0D; c[i++] = 0x0B; c[i++] = 0x0B; c[i++] = colour;
    for (char ch : text) if (i < 38) c[i++] = std::uint8_t(ch);
    c[i++] = 0x0A; c[i++] = 0x0A;
    return c;
}

void testHamming() {
    std::printf("teletext: Hamming 8/4 and parity\n");
    int wrong = 0, uncorrected = 0;
    for (int n = 0; n < 16; ++n) {
        const std::uint8_t b = hamming84(n);
        if (hamData(b) != n || hamming84Decode(b) != n) ++wrong;
        for (int bit = 0; bit < 8; ++bit)
            if (hamming84Decode(std::uint8_t(b ^ (1 << bit))) != n) ++uncorrected;
    }
    CHECK(wrong == 0);
    CHECK(uncorrected == 0);
    CHECK(oddParity('A') == 0xC1 && oddParity(' ') == 0x20 && oddParity(0x0D) == 0x0D);
}

void testTeletextAgainstCapture() {
    std::printf("teletext: packets and OP-47 match a broadcast capture\n");
    // Headers as broadcast kit sent them for page 888 (erase) and 887 (repeat).
    const std::uint8_t h888[10] = {0x15, 0x15, 0xD0, 0xD0, 0x15, 0xD0, 0x15, 0xD0, 0x73, 0x15};
    const std::uint8_t h887[10] = {0x15, 0x15, 0x2F, 0xD0, 0x15, 0x15, 0x15, 0xD0, 0x73, 0x15};
    PageControl erase;
    erase.erase = true;
    CHECK(std::memcmp(headerPacket(0x888, erase).data(), h888, 10) == 0);
    CHECK(std::memcmp(headerPacket(0x887, PageControl{}).data(), h887, 10) == 0);

    // A complete OP-47 user data payload from the same capture: one row-22
    // packet, footer sequence 0x9A01.
    const std::vector<std::uint8_t> real = {
        0x51, 0x15, 0x3A, 0x02, 0x16, 0x00, 0x00, 0x00, 0x00, 0x55, 0x55, 0x27, 0x15, 0x9B,
        0x20, 0x0D, 0x0B, 0x0B, 0xCB, 0x61, 0xF4, 0xE5, 0x2C, 0x20, 0xE3, 0x61, 0x6E, 0x20,
        0x49, 0x20, 0x68, 0x61, 0x76, 0xE5, 0x20, 0x61, 0x20, 0xF7, 0xEF, 0xF2, 0x64, 0x20,
        0xF7, 0xE9, 0xF4, 0x68, 0x20, 0x79, 0xEF, 0x75, 0xBF, 0x8A, 0x8A, 0x20, 0x74, 0x9A,
        0x01, 0x9C};
    std::array<std::uint8_t, 40> cells{};
    for (int i = 0; i < 40; ++i) cells[std::size_t(i)] = real[std::size_t(14 + i)] & 0x7F;
    const TeletextPacket row = rowPacket(8, 22, cells);
    CHECK(row[0] == 0x15 && row[1] == 0x9B);
    CHECK(op47Sdp(std::span<const TeletextPacket>(&row, 1), 0x9A01) == real);

    // Five packets: the length counts every byte and the bytes sum to zero.
    std::vector<TeletextPacket> five(5, row);
    const auto sdp = op47Sdp(five, 0xFFFF);
    unsigned sum = 0;
    for (auto b : sdp) sum += b;
    CHECK(sdp.size() == 9 + 5 * 45 + 4 && sdp[2] == sdp.size());
    CHECK((sum & 0xFF) == 0);
    CHECK(sdp[4] == 18 && sdp[8] == 22);
}

std::vector<Ttx> drain(TeletextPager& pager, int frames, int perFrame = 1) {
    std::vector<Ttx> out;
    for (int f = 0; f < frames; ++f)
        for (const auto& p : pager.takeFrame(perFrame)) out.push_back(parseTtx(p.data()));
    return out;
}

void testPager() {
    std::printf("teletext: page cycles, repeats and erases\n");
    TeletextPager pager(50, 0x888);
    CHECK(drain(pager, 100).empty());   // nothing on air: nothing to send

    // NewforTest's order: CLEAR, BUILD (first row clears), REVEAL. The CLEAR
    // has not gone out before the caption arrives, so the two merge.
    pager.clear();
    const auto r20 = cellsOf("TWO LINE"), r22 = cellsOf("SUBTITLE");
    pager.build(true, 20, r20);
    pager.build(false, 22, r22);
    pager.reveal();
    auto out = drain(pager, 10);
    CHECK(out.size() == 4);
    if (out.size() == 4) {
        CHECK(out[0].packet == 0 && out[0].page == 0x888 && out[0].erase && out[0].subtitle);
        CHECK(out[1].packet == 20 && out[1].text.find("TWO LINE") != std::string::npos);
        CHECK(out[2].packet == 22 && out[2].text.find("SUBTITLE") != std::string::npos);
        CHECK(out[3].packet == 0 && out[3].page == 0x8FF && !out[3].erase);
        // Rows go out with odd parity on every byte.
        int even = 0;
        for (int i = 2; i < 42; ++i) if (__builtin_popcount(out[1].raw[std::size_t(i)]) % 2 == 0) ++even;
        CHECK(even == 0);
    }
    CHECK(pager.stats().coalesced == 1);

    // Repeats: the same rows, C4 clear, 50 frames after the last packet went
    // out (frame 4 of the 10 just drained).
    out = drain(pager, 43);
    CHECK(out.empty());
    out = drain(pager, 4);
    CHECK(out.size() == 4 && !out.empty() && out[0].page == 0x888 && !out[0].erase);

    // An add-on row with no clear bit: C4 clear, only the new row.
    pager.build(false, 18, cellsOf("ADDED"));
    pager.reveal();
    out = drain(pager, 10);
    CHECK(out.size() == 5);   // header, rows 18/20/22, filler: the whole page

    // CLEAR: an erase (C4, no rows), and the erase repeats.
    pager.clear();
    out = drain(pager, 10);
    CHECK(out.size() == 2 && out[0].page == 0x888 && out[0].erase && out[1].page == 0x8FF);
    out = drain(pager, 50);
    CHECK(out.size() == 2 && !out.empty() && out[0].erase);

    // Changing page takes the old page's caption off air first.
    pager.build(true, 22, cellsOf("OLD PAGE"));
    pager.reveal();
    drain(pager, 10);
    pager.connect(0x801);
    out = drain(pager, 10);
    CHECK(out.size() == 2 && !out.empty() && out[0].page == 0x888 && out[0].erase);
    CHECK(pager.page() == 0x801);

    // More packets per frame: a whole cycle in one frame.
    TeletextPager fast(0, 0x888);
    fast.build(true, 22, cellsOf("FAST"));
    fast.reveal();
    CHECK(fast.takeFrame(5).size() == 3);
    CHECK(drain(fast, 200).empty());   // repeat off
}

std::vector<std::uint8_t> newforConnect(int mag, int tens, int units) {
    return {0x0E, 0x00, hamming84(mag), hamming84(tens), hamming84(units)};
}

std::vector<std::uint8_t> newforBuild(bool clear, const std::vector<std::pair<int, std::array<std::uint8_t, 40>>>& rows) {
    std::vector<std::uint8_t> b = {0x8F, hamming84((clear ? 8 : 0) | int(rows.size()))};
    for (const auto& [row, cells] : rows) {
        b.push_back(hamming84(row >> 4));
        b.push_back(hamming84(row & 0x0F));
        for (auto c : cells) b.push_back(oddParity(c));
    }
    return b;
}

void testNewforParser() {
    std::printf("NEWFOR: commands to the pager\n");
    TeletextPager pager(0, 0x100);
    std::vector<std::string> log;
    NewforParser parser(pager, [&](const std::string& s) { log.push_back(s); });

    std::vector<std::uint8_t> stream;
    auto add = [&](const std::vector<std::uint8_t>& b) { stream.insert(stream.end(), b.begin(), b.end()); };
    add(newforConnect(8, 8, 8));
    add({0x98});                                                     // CLEAR
    add(newforBuild(true, {{20, cellsOf("BUT WHAT IF")}}));          // FAB: one row per BUILD
    add(newforBuild(false, {{22, cellsOf("YOU COULD")}}));
    add({0x10});                                                     // REVEAL
    // A byte at a time, as a slow link might deliver it.
    for (auto b : stream) parser.feed({&b, 1});
    CHECK(pager.page() == 0x888);
    CHECK(parser.commands() == 5);
    auto out = drain(pager, 10);
    CHECK(out.size() == 4 && !out.empty() && out[0].erase);
    CHECK(out.size() == 4 && out[1].packet == 20 && out[2].packet == 22 &&
          out[2].text.find("YOU COULD") != std::string::npos);

    // Several rows in one BUILD, then DISCONNECT (page 999) erases it.
    stream.clear();
    add(newforBuild(true, {{18, cellsOf("ONE")}, {20, cellsOf("TWO")}, {22, cellsOf("THREE")}}));
    add({0x10});
    add(newforConnect(9, 9, 9));
    parser.feed(stream);
    CHECK(parser.disconnected());
    out = drain(pager, 10);
    CHECK(out.size() == 2 && !out.empty() && out[0].erase);   // merged: the caption never went out
    CHECK(parser.unknownBytes() == 0);

    // FAB Subtitler opens every session with CONNECT 888 then CONNECT 000
    // (newfor.pcap, 2026-11-11). The page must stay 888.
    std::printf("NEWFOR: FAB's CONNECT 000 keeps the page\n");
    TeletextPager fabPager(0, 0x888);
    NewforParser fab(fabPager, [&](const std::string& s) { log.push_back(s); });
    const std::vector<std::uint8_t> fabOpen = {
        0x0E, 0x15, 0xD0, 0xD0, 0xD0,                                // CONNECT 888
        0x0E, 0x15, 0x15, 0x15, 0x15,                                // CONNECT 000
        0x98,                                                        // CLEAR
    };
    fab.feed(fabOpen);
    stream.clear();
    add(newforBuild(true, {{22, cellsOf("DEMO White/Black SH Bottom Left")}}));
    add({0x10});
    fab.feed(stream);
    CHECK(fabPager.page() == 0x888);
    CHECK(fab.commands() == 5);
    CHECK(!log.empty() && log.back() == "REVEAL");
    out = drain(fabPager, 10);
    CHECK(!out.empty() && out[0].page == 0x888);
}

// The SD slicer, restated: 1.945945 samples a bit, linear interpolation, a
// phase search locking the run-in and framing code.
bool sliceWst(const std::uint16_t* luma, int n, TeletextPacket& out) {
    const double spb = 13.5e6 / 6.9375e6;
    std::uint16_t mn = 1023, mx = 0;
    for (int i = 0; i < n; ++i) { mn = std::min(mn, luma[i]); mx = std::max(mx, luma[i]); }
    if (mx - mn < 200) return false;
    const double mid = (mn + mx) / 2.0;
    auto at = [&](double pos) {
        const int i = int(pos);
        if (i + 1 >= n) return double(luma[n - 1]);
        const double f = pos - i;
        return luma[i] * (1 - f) + luma[i + 1] * f;
    };
    auto bit = [&](int i, double s) { return at(s + i * spb) > mid ? 1 : 0; };
    auto byte = [&](int i, double s) { int v = 0; for (int b = 0; b < 8; ++b) v |= bit(i + b, s) << b; return v; };
    int best = -1;
    double bestStart = 0;
    for (double s = 2.0; s < 14.0; s += 0.02) {
        int score = 0;
        for (int b = 0; b < 15; ++b) score += bit(b, s) != bit(b + 1, s);
        if (byte(16, s) == 0x27) score += 20;
        if (score > best) { best = score; bestStart = s; }
    }
    if (byte(16, bestStart) != 0x27) return false;
    for (int i = 0; i < 42; ++i) out[std::size_t(i)] = std::uint8_t(byte(24 + 8 * i, bestStart));
    return true;
}

void testTeletextInFrame(SdiFormat format, int perFrame) {
    const SdiFormatInfo& fi = formatInfo(format);
    std::printf("teletext in %s, %d packet(s)/frame\n", fi.name, perFrame);
    ComposerSettings s;
    s.format = format;
    s.flashPeriodFrames = 0;
    s.audio.groups = 4;
    s.teletext = std::make_shared<TeletextPager>(0, 0x888);
    s.teletextPacketsPerFrame = perFrame;
    s.teletext->build(true, 20, cellsOf("SUBTITLE ROW 20"));
    s.teletext->build(false, 22, cellsOf("SUBTITLE ROW 22"));
    s.teletext->reveal();
    FrameComposer composer(s);

    const int line = teletextLine(fi, perFrame);
    CHECK(line > 0);
    std::vector<Ttx> got;
    int sequence = -1, sdps = 0, badAnc = 0;
    const int lw = fi.totalSamples * 2, aw = fi.activeWidth * 2;
    for (int k = 0; k < 6; ++k) {
        auto pic = rampPicture(fi);
        UyvyImage img{pic.data(), fi.activeWidth, fi.activeHeight, fi.activeWidth * 2};
        const auto packed = composer.compose(img, std::uint64_t(k), 1000);
        std::vector<std::uint16_t> w(std::size_t(lw) * fi.totalLines);
        unpack10(packed.data(), w.size(), w.data());
        // Timing references, line numbers and CRCs still check out, the CRC
        // now covering the inserted data.
        const WireCheck wc = checkWire(fi, w, pic);
        CHECK(wc.badEav == 0 && wc.badSav == 0 && wc.badLn == 0 && wc.badCrc == 0);
        CHECK(receiverRules(fi, line).v && receiverRules(fi, line).row < 0);
        CHECK(decodeAudio(fi, w).badChecksums == 0);

        const std::uint16_t* active = w.data() + std::size_t(line - 1) * lw + (lw - aw);
        if (fi.isHd) {
            // ANC in the luma stream: ADF, DID 0x43, SDID 0x02, DC, UDW, checksum.
            const std::uint16_t* y = active + 1;
            if (!(y[0] == 0 && y[2] == 0x3FF && y[4] == 0x3FF)) continue;
            ++sdps;
            const int dc = y[10] & 0xFF;
            std::vector<std::uint16_t> words;
            for (int q = 0; q < 3 + dc; ++q) words.push_back(y[2 * (3 + q)]);
            if ((words[0] & 0xFF) != 0x43 || (words[1] & 0xFF) != 0x02) ++badAnc;
            if (ancChecksum(words.data(), words.size()) != y[2 * (6 + dc)]) ++badAnc;
            for (auto x : words) if (!nineBitParityOk(x)) ++badAnc;
            std::vector<std::uint8_t> udw;
            for (int q = 0; q < dc; ++q) udw.push_back(std::uint8_t(words[std::size_t(3 + q)]));
            unsigned sum = 0;
            for (auto b : udw) sum += b;
            if (udw[0] != 0x51 || udw[1] != 0x15 || udw[2] != dc || udw[3] != 0x02 || (sum & 0xFF)) ++badAnc;
            const int seq = (udw[std::size_t(dc - 3)] << 8) | udw[std::size_t(dc - 2)];
            if (sequence >= 0 && seq != sequence + 1) ++badAnc;
            sequence = seq;
            int blocks = 0;
            for (int d = 0; d < 5; ++d) blocks += udw[std::size_t(4 + d)] != 0;
            for (int b = 0; b < blocks; ++b) {
                const std::uint8_t* blk = udw.data() + 9 + 45 * b;
                if (blk[0] != 0x55 || blk[1] != 0x55 || blk[2] != 0x27) { ++badAnc; continue; }
                got.push_back(parseTtx(blk + 3));
            }
        } else {
            for (int l = line; l < line + perFrame; ++l) {
                const std::uint16_t* a = w.data() + std::size_t(l - 1) * lw + (lw - aw);
                std::uint16_t luma[720];
                for (int q = 0; q < 720; ++q) luma[q] = a[1 + 2 * q];
                TeletextPacket p;
                if (sliceWst(luma, 720, p)) got.push_back(parseTtx(p.data()));
                // Never outside the legal range, never on the chroma words.
                for (int q = 0; q < 720; ++q) if (luma[q] < 64 || luma[q] > 940 || a[2 * q] != kBlankChroma) { ++badAnc; break; }
            }
        }
    }
    CHECK(badAnc == 0);
    if (fi.isHd) CHECK(sdps == (4 + perFrame - 1) / perFrame);
    CHECK(got.size() == 4);
    if (got.size() == 4) {
        CHECK(got[0].page == 0x888 && got[0].erase && got[0].subtitle);
        CHECK(got[1].packet == 20 && got[1].text.find("SUBTITLE ROW 20") != std::string::npos);
        CHECK(got[2].packet == 22 && got[2].text.find("SUBTITLE ROW 22") != std::string::npos);
        CHECK(got[3].page == 0x8FF);
    }
}

}  // namespace

int main() {
    testTimecode();
    testMarkerPayload();
    testChannelStatus();
    testBackgroundColour();
    testSt299Ecc();
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
    testHamming();
    testTeletextAgainstCapture();
    testPager();
    testNewforParser();
    testTeletextInFrame(SdiFormat::HD1080i25, 1);
    testTeletextInFrame(SdiFormat::HD1080i25, 5);
    testTeletextInFrame(SdiFormat::HD1080p50, 2);
    testTeletextInFrame(SdiFormat::HD720p50, 1);
    testTeletextInFrame(SdiFormat::HD1080i2997, 1);
    testTeletextInFrame(SdiFormat::SD625i25, 1);
    testTeletextInFrame(SdiFormat::SD625i25, 4);
    CHECK(teletextLine(formatInfo(SdiFormat::SD525i2997), 1) == 0);
    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
