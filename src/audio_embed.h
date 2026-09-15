// Embedded audio: SMPTE ST 299-1 on HD rasters, ST 272 on SD.
//
// Samples are placed by time, not by frame: sample s belongs to the video line
// whose period contains s/48000 seconds, so the audio a frame carries is exactly
// the audio that belongs to that frame. That is what makes a muted frame and a
// flashed frame the same frame on the wire, with no offset to calibrate away.
//
// Channels in every enabled group carry the same tone. Channel status is
// professional, 48 kHz, 24-bit (20-bit on SD), with its CRC.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "pcapreplay/sdi_format.h"
#include "pcapreplay/sdi_raster.h"

namespace testsignal {

inline constexpr int kAudioRateHz = 48000;

struct AudioSettings {
    int    groups = 1;          // 1..4, four channels each
    double toneHz = 1000.0;
    double levelDbfs = -18.0;   // EBU R68 alignment level
};

struct AudioEmbedStats {
    std::uint64_t samples = 0;         // sample periods embedded (per channel)
    std::uint64_t packets = 0;
    std::uint64_t overflowedPackets = 0;   // would not fit a line's HANC; dropped
};

class AudioEmbedder {
public:
    AudioEmbedder(const pcapreplay::SdiFormatInfo& fi, const AudioSettings& settings);

    // First sample period belonging to video frame `frameIndex`.
    static std::uint64_t firstSampleOfFrame(const pcapreplay::SdiFormatInfo& fi,
                                            std::uint64_t frameIndex);

    // The signed 24-bit tone value for sample `n`, before muting.
    std::int32_t toneSample(std::uint64_t n) const;

    // Replace the frame's HANC with this frame's audio. `muted` embeds silence
    // in every channel for the whole frame.
    void embed(pcapreplay::SdiFrameBuilder& builder, std::uint64_t frameIndex, bool muted);

    const AudioEmbedStats& stats() const { return stats_; }

private:
    struct Pending {
        std::uint64_t sample;
        std::uint16_t clockPhase;   // video sample clocks into the line
        bool          movedPastSwitch;
    };

    void embedHdLine(std::span<std::uint16_t> hanc, const std::vector<Pending>& samples,
                     bool muted);
    void embedSdLine(std::span<std::uint16_t> hanc, const std::vector<Pending>& samples,
                     bool muted);
    bool noAudioLine(int line) const;

    pcapreplay::SdiFormatInfo       fi_;
    AudioSettings                   settings_;
    double                          amplitude_ = 0.0;
    std::array<std::uint8_t, 24>    status_{};
    std::array<std::uint8_t, 4>     dbn_{};
    std::vector<std::vector<Pending>> lines_;
    AudioEmbedStats                 stats_;
};

// Professional AES3 channel status for 48 kHz and the given word length, CRC in
// byte 23.
std::array<std::uint8_t, 24> professionalChannelStatus(int wordLengthBits);
std::uint8_t channelStatusCrc(const std::uint8_t* first23);

// ST 299-1 ECC over the 24 words ADF..UDW17 (low eight bits), BCH(31,25) per
// bit plane. See the README: not yet confirmed against third-party kit.
std::array<std::uint8_t, 6> st299Ecc(const std::uint8_t* first24);

// 8-bit data word with even parity in b8 and b9 = !b8, as ST 291 requires.
std::uint16_t ancDataWord(std::uint8_t value);

}  // namespace testsignal
