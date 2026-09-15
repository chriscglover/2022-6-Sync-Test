// One frame of the test signal, from a background picture to packed SDI bytes.
//
// Everything that identifies a frame is decided here from its frame number
// alone: the flash, the audio mute, the burnt-in timecode and the frame
// marker. They cannot disagree about which frame they belong to.
//
// Wire order. PCAP Replay's SdiFrameBuilder lays a line out in ST 274 sample
// numbering -- active picture first, EAV after it -- and flags each SAV for the
// following line. ST 2022-6 equipment cuts a frame at line 1's EAV
// and carry each line as
//
//   [EAV][LN][CRC][HANC][SAV][active picture]
//
// with EAV and SAV both flagged for that line, and the ST 292 CRC covering the
// active picture that precedes the EAV plus the EAV and LN words. The builder is
// used only for the line-number words and the audio in HANC; the wire frame is
// serialised here.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "audio_embed.h"
#include "overlay.h"
#include "pcapreplay/sdi_format.h"
#include "pcapreplay/sdi_raster.h"
#include "timecode.h"

namespace testsignal {

struct ComposerSettings {
    pcapreplay::SdiFormat format = pcapreplay::SdiFormat::HD1080i25;
    AudioSettings audio;
    int           flashPeriodFrames = 50;   // 0 disables the flash and mute
    int           flashFrames = 1;
    std::int64_t  timecodeStartFrames = 0;  // timecode shown on frame 0
    std::string   title = "VIDEO TEST SIGNAL";
    std::uint64_t runTag = 0;
};

// Timing state of one raster line as it goes on the wire.
struct WireLine {
    bool f = false;          // field 2
    bool v = false;          // vertical blanking
    int  activeRow = -1;     // row of the progressive picture, -1 for none
};

// The F/V flags and picture row for `line` (1-based). Follows the format table
// except on 525-line SD, where it uses the SMPTE 125M edges and bottom-field-
// first row order that real NTSC captures carry.
WireLine wireLine(const pcapreplay::SdiFormatInfo& fi, int line);

class FrameComposer {
public:
    explicit FrameComposer(const ComposerSettings& settings);

    const pcapreplay::SdiFormatInfo& info() const { return fi_; }
    const ComposerSettings& settings() const { return settings_; }
    const AudioEmbedder& audio() const { return audio_; }

    bool        isFlash(std::uint64_t frameIndex) const;
    std::string timecodeTextFor(std::uint64_t frameIndex) const;

    // Draws the flash, the text and the marker for `frameIndex` into `picture`
    // (active-picture UYVY). compose() does this first; an SDI output needs
    // only this.
    void drawPicture(UyvyImage picture, std::uint64_t frameIndex, std::uint64_t captureUtcMs);

    // Draws into `picture` (active-picture UYVY) and returns the packed frame,
    // valid until the next call.
    std::span<const std::uint8_t> compose(UyvyImage picture, std::uint64_t frameIndex,
                                          std::uint64_t captureUtcMs);

private:
    void serialise(UyvyImage picture);

    ComposerSettings            settings_;
    pcapreplay::SdiFormatInfo   fi_;
    TimecodeRate                rate_;
    pcapreplay::SdiFrameBuilder builder_;
    AudioEmbedder               audio_;
    std::string                 detailText_;

    int                         lineWords_ = 0;
    std::vector<WireLine>       lines_;      // index 0 = line 1
    std::vector<int>            lineIndex_;  // 0..totalLines-1, for parallel passes
    std::vector<std::uint16_t>  wire_;       // words in wire order
    std::vector<std::uint8_t>   packed_;
};

}  // namespace testsignal
