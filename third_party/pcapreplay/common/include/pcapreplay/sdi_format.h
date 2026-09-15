// SDI raster geometry and the ST 2022-6 HBRMT code points that describe it.
//
// Everything downstream (raster synthesis, packetisation, pacing, the GUI's
// format label) is derived from the table in sdi_format.cpp. Adding a format is
// meant to be a one-row change.
//
// See docs/03-sdi-raster-geometry.md for where these numbers come from and
// docs/02-st2022-6-wire-format.md for the HBRMT code tables.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pcapreplay {

enum class SdiFormat : std::uint8_t {
    Unknown = 0,

    // SD (SMPTE ST 259, 27 MHz word clock, 270 Mbit/s)
    SD625i25,     // 720x576i50  -- 625/50, "PAL"
    SD525i2997,   // 720x486i59.94 -- 525/59.94, "NTSC"

    // HD 1080 (SMPTE ST 274)
    HD1080i25,    // 1080i50
    HD1080i2997,  // 1080i59.94
    HD1080i30,    // 1080i60
    HD1080p25,
    HD1080p2997,
    HD1080p30,
    HD1080p50,    // 3 Gbit/s (ST 424)
    HD1080p5994,  // 3 Gbit/s
    HD1080p60,    // 3 Gbit/s
    HD1080psf25,  // segmented frame, carried with the interlaced raster

    // HD 720 (SMPTE ST 296)
    HD720p50,
    HD720p5994,
    HD720p60,
};

// How the picture is scanned. PsF is carried on the wire exactly like
// interlaced but is flagged differently in HBRMT FRAME and signalled downstream
// as progressive, which is the whole point of keeping it separate.
enum class ScanMode : std::uint8_t { Progressive, Interlaced, SegmentedFrame };

struct SdiFormatInfo {
    SdiFormat   id           = SdiFormat::Unknown;
    const char* name         = "Unknown";

    // Active picture
    int activeWidth          = 0;
    int activeHeight         = 0;

    // Total raster, in *luma* samples per line and lines per frame. The
    // multiplexed Cb/Y/Cr word count is twice totalSamples (4:2:2).
    int totalSamples         = 0;
    int totalLines           = 0;

    // First active line of each field, 1-based, in raster line numbers.
    // For progressive formats only field 1 is used.
    int activeStartF1        = 0;
    int activeStartF2        = 0;   // 0 for progressive
    int activeLinesPerField  = 0;

    // Field boundaries as seen by the EAV/SAV F bit. Field 1 runs
    // [field1StartLine, field2StartLine-1]; field 2 is the rest, wrapping
    // through the end of the raster back to field1StartLine-1. The wrap only
    // actually happens on 525-line SD, where field 1 starts at line 4.
    // Both are 0 for progressive formats.
    int field1StartLine      = 0;
    int field2StartLine      = 0;

    ScanMode scan            = ScanMode::Progressive;

    // Luma sample clock in Hz. Word clock is twice this.
    double lumaClockHz       = 0.0;

    // Frame rate as an exact rational (e.g. 30000/1001).
    int frameRateNum         = 0;
    int frameRateDen         = 1;

    // HBRMT payload header code points.
    std::uint8_t frameCode   = 0;   // FRAME
    std::uint8_t frateCode   = 0;   // FRATE
    std::uint8_t sampleCode  = 0x01;// SAMPLE, 0x01 = 4:2:2 10-bit
    std::uint8_t cfCode      = 0;   // CF, timestamp clock frequency

    // True for ST 292/424 (dual Y/C link with line number + CRC words after
    // EAV); false for ST 259 SD, which has neither LN nor CRC.
    bool isHd                = false;

    // ---- Derived helpers -------------------------------------------------

    // Total 10-bit words in one frame of the multiplexed stream.
    std::int64_t wordsPerFrame() const {
        return std::int64_t(totalSamples) * 2 * totalLines;
    }
    // Bytes per frame of packed 10-bit SDI. Always a whole number of bytes
    // because totalSamples is even for every format in the table (4 words ->
    // 5 bytes), which is asserted at table-validation time.
    std::int64_t bytesPerFrame() const { return wordsPerFrame() * 10 / 8; }

    double frameRate() const {
        return frameRateDen ? double(frameRateNum) / double(frameRateDen) : 0.0;
    }
    // Nominal payload bit rate on one -7 path, excluding IP/UDP/RTP overhead.
    double bitsPerSecond() const { return double(bytesPerFrame()) * 8.0 * frameRate(); }

    bool interlacedOnWire() const { return scan != ScanMode::Progressive; }
    int  fieldsPerFrame()   const { return interlacedOnWire() ? 2 : 1; }
};

// Table access -------------------------------------------------------------

const std::vector<SdiFormatInfo>& allFormats();
const SdiFormatInfo&              formatInfo(SdiFormat f);

// Sender side: pick the SDI format that matches an incoming video frame exactly.
// Returns SdiFormat::Unknown if there is no exact match -- we never scale or
// convert frame rate, so an unknown format is a hard error the GUI surfaces
// rather than something to approximate.
SdiFormat matchFormat(int width, int height,
                      int frameRateNum, int frameRateDen,
                      ScanMode scan);

// Receiver side: recover the format from the HBRMT FRAME/FRATE/SAMPLE codes.
SdiFormat formatFromHbrmtCodes(std::uint8_t frameCode,
                               std::uint8_t frateCode,
                               std::uint8_t sampleCode);

std::string formatDescription(SdiFormat f);   // e.g. "1080i50  1.485 Gb/s"

// For a raster line number (1-based), what the EAV/SAV F and V bits should be.
struct LineFlags {
    bool f = false;   // field 2
    bool v = false;   // vertical blanking (line carries no active picture)
};
LineFlags lineFlags(const SdiFormatInfo& fi, int line);

// Map a raster line to its index within the active picture (0-based row in the
// full progressive frame). Returns -1 for blanking lines.
int activeRowForLine(const SdiFormatInfo& fi, int line);

}  // namespace pcapreplay
