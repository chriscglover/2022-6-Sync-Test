// Drawing into an 8-bit UYVY picture: rectangles, block text and the frame
// marker.
//
// Text is a built-in 5x7 font rather than Pango so the burnt-in timecode is
// drawn in the same thread, from the same frame number, as the audio and the
// flash -- nothing can put a different frame's timecode on a picture.
#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace testsignal {

struct UyvyImage {
    std::uint8_t* data = nullptr;
    int width = 0;
    int height = 0;
    int stride = 0;      // bytes per row, normally width * 2
};

inline constexpr std::uint8_t kBlackY  = 16;
inline constexpr std::uint8_t kWhiteY  = 235;
inline constexpr std::uint8_t kNeutralC = 128;

void fillRect(UyvyImage img, int x, int y, int w, int h,
              std::uint8_t luma, std::uint8_t cb, std::uint8_t cr);
void fillFrame(UyvyImage img, std::uint8_t luma, std::uint8_t cb, std::uint8_t cr);

// Width in pixels of `text` at `scale` pixels per font cell.
int  textWidth(const std::string& text, int scale);
inline int textHeight(int scale) { return 7 * scale; }

// Largest scale no greater than `wanted` at which `text` fits `maxWidth`.
int  fitScale(const std::string& text, int wanted, int maxWidth);

// Upper-cased; characters outside the font draw as spaces.
void drawText(UyvyImage img, int x, int y, int scale, const std::string& text,
              std::uint8_t luma);

// White text on a black box, centred horizontally with its top at `y`.
// Returns the box height.
int  drawLabel(UyvyImage img, int y, int wantedScale, const std::string& text);

// ---- frame marker -------------------------------------------------------------
//
// A 36x22 cell grid in the bottom-left corner carrying a CRC-protected run tag,
// frame number and capture time: 24 bytes ("MV", version 1, flags, run tag,
// frame number, 48-bit UTC milliseconds, CRC-16/CCITT-FALSE), each bit
// repeated three times inside a finder border. It survives scaling, so it can
// be read at every hop that keeps the corner of the picture.
std::array<std::uint8_t, 24> markerPayload(std::uint64_t runTag, std::uint32_t frameNumber,
                                           std::uint64_t captureUtcMs);
void drawMarker(UyvyImage img, std::uint64_t runTag, std::uint32_t frameNumber,
                std::uint64_t captureUtcMs);

}  // namespace testsignal
