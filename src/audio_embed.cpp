#include "audio_embed.h"

#include <algorithm>
#include <bit>
#include <cmath>

#include "pcapreplay/crc.h"

namespace testsignal {

using namespace pcapreplay;

namespace {

constexpr double kPi = 3.14159265358979323846;

int parity(std::uint32_t v) { return std::popcount(v) & 1; }

// ST 299-1 data DIDs: group 1..4 = E7, E6, E5, E4. ST 272: FF, FD, FB, F9.
std::uint8_t hdDid(int group) { return std::uint8_t(0xE7 - group); }
std::uint8_t sdDid(int group) { return std::uint8_t(0xFF - 2 * group); }

void fillBlanking(std::span<std::uint16_t> span) {
    for (std::size_t i = 0; i < span.size(); ++i)
        span[i] = (i & 1) ? kBlankLuma : kBlankChroma;
}

}  // namespace

std::uint16_t ancDataWord(std::uint8_t value) {
    return withNotB8(std::uint16_t(value | (parity(value) << 8)));
}

std::uint8_t channelStatusCrc(const std::uint8_t* first23) {
    std::uint8_t crc = 0xFF;
    for (int i = 0; i < 23; ++i)
        for (int bit = 0; bit < 8; ++bit) {
            const bool feedback = ((crc ^ (first23[i] >> bit)) & 1) != 0;
            crc = std::uint8_t(crc >> 1);
            if (feedback) crc ^= 0xB8;
        }
    return crc;
}

std::array<std::uint8_t, 24> professionalChannelStatus(int wordLengthBits) {
    std::array<std::uint8_t, 24> b{};
    // Byte 0: professional, audio, no emphasis (code 1), locked, 48 kHz.
    b[0] = 0x01 | (1 << 2) | 0x80;
    // Byte 2: auxiliary bits carry audio (24-bit max) or not (20-bit max); the
    // word length is the maximum for that mode (code 5).
    b[2] = std::uint8_t((wordLengthBits > 20 ? 4 : 2) | (5 << 3));
    b[23] = channelStatusCrc(b.data());
    return b;
}

std::array<std::uint8_t, 6> st299Ecc(const std::uint8_t* first24) {
    // g(x) = (x + 1)(x^5 + x^2 + 1) = x^6 + x^5 + x^3 + x^2 + x + 1.
    constexpr std::uint8_t kFeedback = 0x2F;   // g(x) without x^6
    std::array<std::uint8_t, 6> ecc{};
    for (int plane = 0; plane < 8; ++plane) {
        std::uint8_t reg = 0;
        for (int i = 0; i < 24; ++i) {
            const int in = (first24[i] >> plane) & 1;
            const int feedback = ((reg >> 5) & 1) ^ in;
            reg = std::uint8_t((reg << 1) & 0x3F);
            if (feedback) reg ^= kFeedback;
        }
        for (int k = 0; k < 6; ++k)
            if (reg & (1 << k)) ecc[std::size_t(k)] |= std::uint8_t(1 << plane);
    }
    return ecc;
}

AudioEmbedder::AudioEmbedder(const SdiFormatInfo& fi, const AudioSettings& settings)
    : fi_(fi), settings_(settings) {
    settings_.groups = std::clamp(settings_.groups, 1, 4);
    for (int c = 0; c < 16; ++c)
        amplitudes_[std::size_t(c)] = std::pow(10.0, settings_.channelDbfs(c) / 20.0) * double((1 << 23) - 1);
    status_ = professionalChannelStatus(fi_.isHd ? 24 : 20);
    dbn_.fill(1);
    lines_.resize(std::size_t(fi_.totalLines) + 2);
}

std::uint64_t AudioEmbedder::firstSampleOfFrame(const SdiFormatInfo& fi,
                                                std::uint64_t frameIndex) {
    // The first sample at or after the frame's start time. Rounding down would
    // hand a frame the last sample of the previous one on non-integer rates.
    const unsigned __int128 n = (unsigned __int128)frameIndex * kAudioRateHz *
                                std::uint64_t(fi.frameRateDen);
    const std::uint64_t num = std::uint64_t(fi.frameRateNum);
    return std::uint64_t((n + num - 1) / num);
}

std::int32_t AudioEmbedder::toneSample(std::uint64_t n, int channel) const {
    const double cycles = settings_.toneHz * double(n % std::uint64_t(kAudioRateHz * 1000)) /
                          double(kAudioRateHz);
    const double amplitude = amplitudes_[std::size_t(std::clamp(channel, 0, 15))];
    return std::int32_t(std::lround(amplitude * std::sin(2.0 * kPi * cycles)));
}

int AudioEmbedder::maxSamplesPerLine() const {
    const int lineWords = fi_.totalSamples * 2;
    const int hanc = lineWords - fi_.activeWidth * 2 - postActiveWords(fi_) - savWords(fi_);
    const int groups = settings_.groups;
    if (fi_.isHd) {
        // One 31-word ST 299 packet per sample per group, on alternate words.
        return std::max(1, (hanc / 2) / (31 * groups));
    }
    // One ST 272 packet per group per line: 7 words plus 12 per sample.
    return std::max(1, (hanc / groups - 7) / 12);
}

bool AudioEmbedder::noAudioLine(int line) const {
    // Audio is not carried on the line after a switching line.
    switch (fi_.totalLines) {
        case 1125: return line == 8 || (fi_.interlacedOnWire() && line == 570);
        case 750:  return line == 8;
        case 625:  return line == 7 || line == 320;
        case 525:  return line == 11 || line == 274;
        default:   return false;
    }
}

void AudioEmbedder::embed(SdiFrameBuilder& builder, std::uint64_t frameIndex, bool muted) {
    const std::uint64_t first = firstSampleOfFrame(fi_, frameIndex);
    const std::uint64_t end   = firstSampleOfFrame(fi_, frameIndex + 1);

    for (auto& l : lines_) l.clear();

    // Position of sample s within the frame, in units of 1/(48000*den) s:
    // r = s*num - frame*48000*den lies in [0, 48000*den).
    const std::uint64_t q = std::uint64_t(kAudioRateHz) * std::uint64_t(fi_.frameRateDen);
    const unsigned __int128 frameOrigin = (unsigned __int128)frameIndex * q;
    for (std::uint64_t s = first; s < end; ++s) {
        const unsigned __int128 at = (unsigned __int128)s * std::uint64_t(fi_.frameRateNum);
        const std::uint64_t r = std::uint64_t(at - frameOrigin);
        const unsigned __int128 scaled = (unsigned __int128)r * std::uint64_t(fi_.totalLines);
        int line = int(scaled / q) + 1;
        const std::uint64_t within = std::uint64_t(scaled % q);
        const std::uint16_t phase =
            std::uint16_t((unsigned __int128)within * std::uint64_t(fi_.totalSamples) / q);
        bool moved = false;
        if (noAudioLine(line)) { ++line; moved = true; }
        line = std::min(line, fi_.totalLines);
        lines_[std::size_t(line)].push_back({s, phase, moved});
    }

    // A line's HANC holds only so many packets. With all four groups, the line
    // after a switching point (which takes the skipped line's samples too) can
    // exceed it on 1080i59.94, 720p59.94 and SD, so carry the excess forward to
    // the following lines, which on average carry well under their capacity.
    const std::size_t capacity = std::size_t(maxSamplesPerLine());
    std::vector<Pending> carry;
    for (int ln = 1; ln <= fi_.totalLines; ++ln) {
        auto& samples = lines_[std::size_t(ln)];
        if (!carry.empty()) {
            for (auto& p : carry) p.movedPastSwitch = true;
            samples.insert(samples.begin(), carry.begin(), carry.end());
            carry.clear();
        }
        if (samples.size() > capacity) {
            carry.assign(samples.begin() + std::ptrdiff_t(capacity), samples.end());
            samples.resize(capacity);
        }
    }
    stats_.overflowedPackets += carry.size() * std::size_t(settings_.groups);

    for (int ln = 1; ln <= fi_.totalLines; ++ln) {
        std::span<std::uint16_t> hanc = builder.hancSpan(ln);
        fillBlanking(hanc);
        const auto& samples = lines_[std::size_t(ln)];
        if (samples.empty()) continue;
        if (fi_.isHd) embedHdLine(hanc, samples, muted);
        else          embedSdLine(hanc, samples, muted);
    }
    stats_.samples += end - first;
}

void AudioEmbedder::embedHdLine(std::span<std::uint16_t> hanc,
                                const std::vector<Pending>& samples, bool muted) {
    // ST 299-1 packets ride the colour-difference stream: every other word,
    // starting at the first HANC word (which is a Cb/Cr position).
    std::size_t at = 0;
    constexpr std::size_t kPacketWords = 3 + 3 + 24 + 1;
    for (const Pending& p : samples) {
        for (int group = 0; group < settings_.groups; ++group) {
            if (at + 2 * kPacketWords > hanc.size()) { ++stats_.overflowedPackets; continue; }

            std::uint8_t udw[24] = {};
            udw[0] = std::uint8_t(p.clockPhase & 0xFF);
            udw[1] = std::uint8_t(((p.clockPhase >> 8) & 0x0F) | (((p.clockPhase >> 12) & 1) << 5) |
                                  (p.movedPastSwitch ? 0x10 : 0));

            const int blockPos = int(p.sample % 192);
            const bool c = (status_[std::size_t(blockPos / 8)] >> (blockPos % 8)) & 1;
            for (int ch = 0; ch < 4; ++ch) {
                const std::int32_t value = muted ? 0 : toneSample(p.sample, group * 4 + ch);
                // AES subframe time slots 4..31: 24 audio bits, V, U, C, P.
                std::uint32_t sub = (std::uint32_t(value) & 0xFFFFFFu) << 4;
                if (c) sub |= 1u << 30;
                if (parity(sub >> 4)) sub |= 1u << 31;
                const int base = 2 + ch * 4;
                for (int b = 0; b < 4; ++b)
                    udw[base + b] = std::uint8_t(sub >> (8 * b));
                // Z (start of the 192-sample block) is flagged in the first
                // byte of channels 1 and 3 only.
                if (ch % 2 == 0 && blockPos == 0) udw[base] |= 0x08;
            }

            const std::uint8_t did = hdDid(group);
            const std::uint8_t dbn = dbn_[std::size_t(group)];
            dbn_[std::size_t(group)] = std::uint8_t(dbn == 255 ? 1 : dbn + 1);

            std::uint8_t eccIn[24] = {0x00, 0xFF, 0xFF, did, dbn, 24};
            for (int i = 0; i < 18; ++i) eccIn[6 + i] = udw[i];
            const auto ecc = st299Ecc(eccIn);
            for (int i = 0; i < 6; ++i) udw[18 + i] = ecc[std::size_t(i)];

            std::uint16_t words[kPacketWords];
            words[0] = 0x000; words[1] = 0x3FF; words[2] = 0x3FF;
            words[3] = ancDataWord(did);
            words[4] = ancDataWord(dbn);
            words[5] = ancDataWord(24);
            for (int i = 0; i < 24; ++i) words[6 + i] = ancDataWord(udw[i]);
            words[30] = ancChecksum(words + 3, 27);

            for (std::size_t i = 0; i < kPacketWords; ++i) hanc[at + 2 * i] = words[i];
            at += 2 * kPacketWords;
            ++stats_.packets;
        }
    }
}

void AudioEmbedder::embedSdLine(std::span<std::uint16_t> hanc,
                                const std::vector<Pending>& samples, bool muted) {
    // ST 272: one packet per group per line, three words per channel sample.
    std::size_t at = 0;
    const std::size_t dc = samples.size() * 4 * 3;
    const std::size_t packetWords = 6 + dc + 1;
    for (int group = 0; group < settings_.groups; ++group) {
        if (dc > 255 || at + packetWords > hanc.size()) { ++stats_.overflowedPackets; continue; }

        const std::uint8_t did = sdDid(group);
        const std::uint8_t dbn = dbn_[std::size_t(group)];
        dbn_[std::size_t(group)] = std::uint8_t(dbn == 255 ? 1 : dbn + 1);

        std::uint16_t* w = hanc.data() + at;
        w[0] = 0x000; w[1] = 0x3FF; w[2] = 0x3FF;
        w[3] = ancDataWord(did);
        w[4] = ancDataWord(dbn);
        w[5] = ancDataWord(std::uint8_t(dc));
        std::size_t k = 6;
        for (const Pending& p : samples) {
            const int blockPos = int(p.sample % 192);
            const std::uint32_t c = (status_[std::size_t(blockPos / 8)] >> (blockPos % 8)) & 1;
            for (int ch = 0; ch < 4; ++ch) {
                const std::int32_t value = muted ? 0 : toneSample(p.sample, group * 4 + ch);
                const std::uint32_t s20 = std::uint32_t(value >> 4) & 0xFFFFFu;
                std::uint32_t x0 = (blockPos == 0 ? 1u : 0u) | (std::uint32_t(ch) << 1) |
                                   ((s20 & 0x3F) << 3);
                std::uint32_t x1 = (s20 >> 6) & 0x1FF;
                std::uint32_t x2 = ((s20 >> 15) & 0x1F) | (c << 7);
                // P: even parity over the 26 bits before it.
                if (parity(x0 & 0x1FF) ^ parity(x1 & 0x1FF) ^ parity(x2 & 0xFF)) x2 |= 1u << 8;
                w[k++] = withNotB8(std::uint16_t(x0));
                w[k++] = withNotB8(std::uint16_t(x1));
                w[k++] = withNotB8(std::uint16_t(x2));
            }
        }
        w[k] = ancChecksum(w + 3, 3 + dc);
        at += packetWords;
        ++stats_.packets;
    }
}

}  // namespace testsignal
