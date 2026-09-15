#include "pcapreplay/sdi_format.h"

#include <algorithm>
#include <cstdio>
#include <mutex>

namespace pcapreplay {
namespace {

// HBRMT FRAME codes (ST 2022-6 Table 4)
constexpr std::uint8_t FRAME_720x486i   = 0x10;
constexpr std::uint8_t FRAME_720x576i   = 0x11;
constexpr std::uint8_t FRAME_1920x1080i = 0x20;
constexpr std::uint8_t FRAME_1920x1080p = 0x21;
constexpr std::uint8_t FRAME_1920x1080psf = 0x22;
constexpr std::uint8_t FRAME_1280x720p  = 0x30;

// HBRMT FRATE codes (ST 2022-6 Table 5)
constexpr std::uint8_t FRATE_60      = 0x10;
constexpr std::uint8_t FRATE_60_1001 = 0x11;
constexpr std::uint8_t FRATE_50      = 0x12;
constexpr std::uint8_t FRATE_30      = 0x16;
constexpr std::uint8_t FRATE_30_1001 = 0x17;
constexpr std::uint8_t FRATE_25      = 0x18;

// HBRMT SAMPLE codes (ST 2022-6 Table 6)
constexpr std::uint8_t SAMPLE_422_10 = 0x01;

// HBRMT CF (clock frequency of the video timestamp)
constexpr std::uint8_t CF_27MHZ      = 0x01;
constexpr std::uint8_t CF_148_5MHZ   = 0x02;
constexpr std::uint8_t CF_148_5_1001 = 0x03;

constexpr double CLK_27    = 27.0e6;
constexpr double CLK_27_M  = 27.0e6 / 1.001;
constexpr double CLK_74_25 = 74.25e6;
constexpr double CLK_74_M  = 74.25e6 / 1.001;
constexpr double CLK_148_5 = 148.5e6;
constexpr double CLK_148_M = 148.5e6 / 1.001;

// ---------------------------------------------------------------------------
// The table.
//
// Note on FRATE for interlaced formats: we send the *frame* rate, not the field
// rate, so 1080i50 carries FRATE=25 (0x18). FRAME already says "interlaced", so
// frame rate is the non-redundant reading and is what the raster maths implies
// (2640 x 1125 x 74.25 MHz = 25 Hz). This is called out as an interop check in
// docs/07-validation-and-open-questions.md.
// ---------------------------------------------------------------------------
const std::vector<SdiFormatInfo>& table() {
    static const std::vector<SdiFormatInfo> t = [] {
        std::vector<SdiFormatInfo> v;

        // --- SD, ST 259, 27 MHz word clock (13.5 MHz luma) ------------------
        v.push_back({SdiFormat::SD625i25, "625i25 (576i50)",
                     720, 576,  864, 625,
                     /*actF1*/23, /*actF2*/336, /*linesPerField*/288,
                     /*f1Start*/1, /*f2Start*/313,
                     ScanMode::Interlaced, 13.5e6, 25, 1,
                     FRAME_720x576i, FRATE_25, SAMPLE_422_10, CF_27MHZ,
                     /*isHd*/false});

        v.push_back({SdiFormat::SD525i2997, "525i29.97 (486i59.94)",
                     720, 486,  858, 525,
                     /*actF1*/21, /*actF2*/283, /*linesPerField*/243,
                     /*f1Start*/4, /*f2Start*/266,
                     // 13.5 MHz exactly: BT.601 uses a common luma clock for
                     // 525 and 625, and the 1.001 falls out of the 858x525
                     // raster rather than the clock.
                     ScanMode::Interlaced, 13.5e6, 30000, 1001,
                     FRAME_720x486i, FRATE_30_1001, SAMPLE_422_10, CF_27MHZ,
                     /*isHd*/false});

        // --- 1080 interlaced, ST 274 ---------------------------------------
        v.push_back({SdiFormat::HD1080i25, "1080i25 (1080i50)",
                     1920, 1080, 2640, 1125,
                     21, 584, 540, 1, 564,
                     ScanMode::Interlaced, CLK_74_25, 25, 1,
                     FRAME_1920x1080i, FRATE_25, SAMPLE_422_10, CF_148_5MHZ, true});

        v.push_back({SdiFormat::HD1080i2997, "1080i29.97 (1080i59.94)",
                     1920, 1080, 2200, 1125,
                     21, 584, 540, 1, 564,
                     ScanMode::Interlaced, CLK_74_M, 30000, 1001,
                     FRAME_1920x1080i, FRATE_30_1001, SAMPLE_422_10, CF_148_5_1001, true});

        v.push_back({SdiFormat::HD1080i30, "1080i30 (1080i60)",
                     1920, 1080, 2200, 1125,
                     21, 584, 540, 1, 564,
                     ScanMode::Interlaced, CLK_74_25, 30, 1,
                     FRAME_1920x1080i, FRATE_30, SAMPLE_422_10, CF_148_5MHZ, true});

        // --- 1080 PsF (interlaced raster, progressive content) -------------
        v.push_back({SdiFormat::HD1080psf25, "1080PsF25",
                     1920, 1080, 2640, 1125,
                     21, 584, 540, 1, 564,
                     ScanMode::SegmentedFrame, CLK_74_25, 25, 1,
                     FRAME_1920x1080psf, FRATE_25, SAMPLE_422_10, CF_148_5MHZ, true});

        // --- 1080 progressive, ST 274 (HD-SDI rates) -----------------------
        v.push_back({SdiFormat::HD1080p25, "1080p25",
                     1920, 1080, 2640, 1125,
                     42, 0, 1080, 0, 0,
                     ScanMode::Progressive, CLK_74_25, 25, 1,
                     FRAME_1920x1080p, FRATE_25, SAMPLE_422_10, CF_148_5MHZ, true});

        v.push_back({SdiFormat::HD1080p2997, "1080p29.97",
                     1920, 1080, 2200, 1125,
                     42, 0, 1080, 0, 0,
                     ScanMode::Progressive, CLK_74_M, 30000, 1001,
                     FRAME_1920x1080p, FRATE_30_1001, SAMPLE_422_10, CF_148_5_1001, true});

        v.push_back({SdiFormat::HD1080p30, "1080p30",
                     1920, 1080, 2200, 1125,
                     42, 0, 1080, 0, 0,
                     ScanMode::Progressive, CLK_74_25, 30, 1,
                     FRAME_1920x1080p, FRATE_30, SAMPLE_422_10, CF_148_5MHZ, true});

        // --- 1080 progressive, ST 424 (3 Gbit/s) ---------------------------
        v.push_back({SdiFormat::HD1080p50, "1080p50",
                     1920, 1080, 2640, 1125,
                     42, 0, 1080, 0, 0,
                     ScanMode::Progressive, CLK_148_5, 50, 1,
                     FRAME_1920x1080p, FRATE_50, SAMPLE_422_10, CF_148_5MHZ, true});

        v.push_back({SdiFormat::HD1080p5994, "1080p59.94",
                     1920, 1080, 2200, 1125,
                     42, 0, 1080, 0, 0,
                     ScanMode::Progressive, CLK_148_M, 60000, 1001,
                     FRAME_1920x1080p, FRATE_60_1001, SAMPLE_422_10, CF_148_5_1001, true});

        v.push_back({SdiFormat::HD1080p60, "1080p60",
                     1920, 1080, 2200, 1125,
                     42, 0, 1080, 0, 0,
                     ScanMode::Progressive, CLK_148_5, 60, 1,
                     FRAME_1920x1080p, FRATE_60, SAMPLE_422_10, CF_148_5MHZ, true});

        // --- 720 progressive, ST 296 ---------------------------------------
        v.push_back({SdiFormat::HD720p50, "720p50",
                     1280, 720, 1980, 750,
                     26, 0, 720, 0, 0,
                     ScanMode::Progressive, CLK_74_25, 50, 1,
                     FRAME_1280x720p, FRATE_50, SAMPLE_422_10, CF_148_5MHZ, true});

        v.push_back({SdiFormat::HD720p5994, "720p59.94",
                     1280, 720, 1650, 750,
                     26, 0, 720, 0, 0,
                     ScanMode::Progressive, CLK_74_M, 60000, 1001,
                     FRAME_1280x720p, FRATE_60_1001, SAMPLE_422_10, CF_148_5_1001, true});

        v.push_back({SdiFormat::HD720p60, "720p60",
                     1280, 720, 1650, 750,
                     26, 0, 720, 0, 0,
                     ScanMode::Progressive, CLK_74_25, 60, 1,
                     FRAME_1280x720p, FRATE_60, SAMPLE_422_10, CF_148_5MHZ, true});

        return v;
    }();
    return t;
}

}  // namespace

const std::vector<SdiFormatInfo>& allFormats() { return table(); }

const SdiFormatInfo& formatInfo(SdiFormat f) {
    static const SdiFormatInfo unknown{};
    for (const auto& fi : table())
        if (fi.id == f) return fi;
    return unknown;
}

SdiFormat matchFormat(int width, int height,
                      int frameRateNum, int frameRateDen,
                      ScanMode scan) {
    if (width <= 0 || height <= 0 || frameRateNum <= 0 || frameRateDen <= 0)
        return SdiFormat::Unknown;

    // Compare rates as exact cross-products so 30000/1001 and 60000/2002 match.
    const std::int64_t lhs = std::int64_t(frameRateNum);
    const std::int64_t rhsDen = std::int64_t(frameRateDen);

    for (const auto& fi : table()) {
        if (fi.activeWidth != width || fi.activeHeight != height) continue;
        if (fi.scan != scan) continue;
        if (lhs * fi.frameRateDen != std::int64_t(fi.frameRateNum) * rhsDen) continue;
        return fi.id;
    }
    return SdiFormat::Unknown;
}

SdiFormat formatFromHbrmtCodes(std::uint8_t frameCode,
                               std::uint8_t frateCode,
                               std::uint8_t sampleCode) {
    for (const auto& fi : table()) {
        if (fi.frameCode == frameCode &&
            fi.frateCode == frateCode &&
            fi.sampleCode == sampleCode)
            return fi.id;
    }
    return SdiFormat::Unknown;
}

std::string formatDescription(SdiFormat f) {
    const auto& fi = formatInfo(f);
    if (fi.id == SdiFormat::Unknown) return "Unknown format";

    char buf[160];
    const double gbps = fi.bitsPerSecond() / 1.0e9;
    if (gbps >= 1.0)
        std::snprintf(buf, sizeof buf, "%s  %dx%d  %.3f Gb/s", fi.name,
                      fi.activeWidth, fi.activeHeight, gbps);
    else
        std::snprintf(buf, sizeof buf, "%s  %dx%d  %.0f Mb/s", fi.name,
                      fi.activeWidth, fi.activeHeight, gbps * 1000.0);
    return buf;
}

LineFlags lineFlags(const SdiFormatInfo& fi, int line) {
    LineFlags lf;
    if (line < 1 || line > fi.totalLines) return lf;

    if (fi.interlacedOnWire()) {
        // Field 1 is [field1StartLine, field2StartLine-1]; field 2 is the
        // remainder, wrapping past the end of the raster (525-line SD only).
        if (fi.field1StartLine <= 1) {
            lf.f = (line >= fi.field2StartLine);
        } else {
            lf.f = (line >= fi.field2StartLine) || (line < fi.field1StartLine);
        }

        const bool inF1 = line >= fi.activeStartF1 &&
                          line < fi.activeStartF1 + fi.activeLinesPerField;
        const bool inF2 = line >= fi.activeStartF2 &&
                          line < fi.activeStartF2 + fi.activeLinesPerField;
        lf.v = !(inF1 || inF2);
    } else {
        lf.f = false;
        lf.v = !(line >= fi.activeStartF1 &&
                 line < fi.activeStartF1 + fi.activeLinesPerField);
    }
    return lf;
}

int activeRowForLine(const SdiFormatInfo& fi, int line) {
    if (line < 1 || line > fi.totalLines) return -1;

    if (!fi.interlacedOnWire()) {
        const int row = line - fi.activeStartF1;
        return (row >= 0 && row < fi.activeHeight) ? row : -1;
    }

    // Field 1 carries the even rows of the frame, field 2 the odd rows.
    const int r1 = line - fi.activeStartF1;
    if (r1 >= 0 && r1 < fi.activeLinesPerField) return r1 * 2;

    const int r2 = line - fi.activeStartF2;
    if (r2 >= 0 && r2 < fi.activeLinesPerField) return r2 * 2 + 1;

    return -1;
}

}  // namespace pcapreplay
