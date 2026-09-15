#include "overlay.h"

#include <algorithm>
#include <cctype>

namespace testsignal {

namespace {

// 5x7 glyphs, one byte per row, bit 4 the leftmost column.
struct Glyph {
    char ch;
    std::uint8_t rows[7];
};

constexpr Glyph kFont[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
    {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
    {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
    {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
    {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
    {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {':', {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}},
    {';', {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x04, 0x08}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}},
    {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
    {'+', {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00}},
    {'/', {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10}},
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
    {'D', {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
    {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}},
    {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
    {'N', {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}},
    {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04}},
    {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
};

const std::uint8_t* glyphRows(char c) {
    const char up = char(std::toupper(static_cast<unsigned char>(c)));
    for (const auto& g : kFont)
        if (g.ch == up) return g.rows;
    return kFont[0].rows;
}

// ---- frame marker constants ----------------------------------------------------
constexpr int kPayloadBytes = 24;
constexpr int kDataColumns  = 32;
constexpr int kDataRows     = 18;
constexpr int kGridColumns  = 36;
constexpr int kGridRows     = 22;
constexpr int kDataX        = 2;
constexpr int kDataY        = 2;
constexpr std::uint64_t kCaptureMsMask = (std::uint64_t(1) << 48) - 1;

std::uint16_t crc16CcittFalse(const std::uint8_t* data, int len) {
    std::uint16_t crc = 0xFFFF;
    for (int i = 0; i < len; ++i) {
        crc = std::uint16_t(crc ^ (std::uint16_t(data[i]) << 8));
        for (int b = 0; b < 8; ++b)
            crc = (crc & 0x8000) ? std::uint16_t((crc << 1) ^ 0x1021) : std::uint16_t(crc << 1);
    }
    return crc;
}

}  // namespace

void fillRect(UyvyImage img, int x, int y, int w, int h,
              std::uint8_t luma, std::uint8_t cb, std::uint8_t cr) {
    const int x0 = std::clamp(x, 0, img.width);
    const int x1 = std::clamp(x + w, 0, img.width);
    const int y0 = std::clamp(y, 0, img.height);
    const int y1 = std::clamp(y + h, 0, img.height);
    if (x0 >= x1 || y0 >= y1) return;
    // Chroma is shared by a pixel pair, so it covers every pair the span touches.
    const int p0 = x0 / 2, p1 = (x1 + 1) / 2;
    for (int row = y0; row < y1; ++row) {
        std::uint8_t* r = img.data + std::size_t(row) * std::size_t(img.stride);
        for (int px = x0; px < x1; ++px) r[2 * px + 1] = luma;
        for (int p = p0; p < p1; ++p) {
            r[4 * p]     = cb;
            r[4 * p + 2] = cr;
        }
    }
}

void fillFrame(UyvyImage img, std::uint8_t luma, std::uint8_t cb, std::uint8_t cr) {
    fillRect(img, 0, 0, img.width, img.height, luma, cb, cr);
}

int textWidth(const std::string& text, int scale) {
    if (text.empty()) return 0;
    return int(text.size()) * 6 * scale - scale;
}

int fitScale(const std::string& text, int wanted, int maxWidth) {
    int scale = std::max(1, wanted);
    while (scale > 1 && textWidth(text, scale) > maxWidth) --scale;
    return scale;
}

void drawText(UyvyImage img, int x, int y, int scale, const std::string& text,
              std::uint8_t luma) {
    for (std::size_t i = 0; i < text.size(); ++i) {
        const std::uint8_t* rows = glyphRows(text[i]);
        const int gx = x + int(i) * 6 * scale;
        for (int ry = 0; ry < 7; ++ry)
            for (int cx = 0; cx < 5; ++cx)
                if (rows[ry] & (0x10 >> cx))
                    fillRect(img, gx + cx * scale, y + ry * scale, scale, scale,
                             luma, kNeutralC, kNeutralC);
    }
}

int drawLabel(UyvyImage img, int y, int wantedScale, const std::string& text) {
    const int pad = std::max(2, wantedScale);
    const int scale = fitScale(text, wantedScale, img.width - 4 * pad);
    const int w = textWidth(text, scale);
    const int x = (img.width - w) / 2;
    const int boxH = textHeight(scale) + 2 * pad;
    fillRect(img, x - pad, y, w + 2 * pad, boxH, kBlackY, kNeutralC, kNeutralC);
    drawText(img, x, y + pad, scale, text, kWhiteY);
    return boxH;
}

std::array<std::uint8_t, 24> markerPayload(std::uint64_t runTag, std::uint32_t frameNumber,
                                           std::uint64_t captureUtcMs) {
    std::array<std::uint8_t, 24> p{};
    p[0] = 'M';
    p[1] = 'V';
    p[2] = 1;   // version
    p[3] = 0;   // flags
    for (int i = 0; i < 8; ++i) p[4 + i] = std::uint8_t(runTag >> (56 - 8 * i));
    for (int i = 0; i < 4; ++i) p[12 + i] = std::uint8_t(frameNumber >> (24 - 8 * i));
    const std::uint64_t ms = captureUtcMs & kCaptureMsMask;
    for (int i = 0; i < 6; ++i) p[16 + i] = std::uint8_t(ms >> (40 - 8 * i));
    const std::uint16_t crc = crc16CcittFalse(p.data(), 22);
    p[22] = std::uint8_t(crc >> 8);
    p[23] = std::uint8_t(crc);
    return p;
}

void drawMarker(UyvyImage img, std::uint64_t runTag, std::uint32_t frameNumber,
                std::uint64_t captureUtcMs) {
    const int cell = std::min(img.width / 320, img.height / 180);
    if (cell == 0) return;

    const auto payload = markerPayload(runTag, frameNumber, captureUtcMs);
    bool cells[kGridColumns * kGridRows] = {};
    for (int x = 0; x < kGridColumns; ++x) {
        cells[x] = true;
        cells[(kGridRows - 1) * kGridColumns + x] = (x % 2 == 0);
    }
    for (int y = 0; y < kGridRows; ++y) {
        cells[y * kGridColumns] = true;
        cells[y * kGridColumns + kGridColumns - 1] = (y % 2 == 0);
    }
    for (int bit = 0; bit < kPayloadBytes * 8; ++bit) {
        const bool on = payload[std::size_t(bit / 8)] & (1 << (7 - bit % 8));
        for (int copy = 0; copy < 3; ++copy) {
            const int repeated = bit * 3 + copy;
            const int x = kDataX + repeated % kDataColumns;
            const int y = kDataY + repeated / kDataColumns;
            cells[y * kGridColumns + x] = on;
        }
    }

    const int originX = 2 * cell;
    const int originY = img.height - (kGridRows + 2) * cell;
    for (int gy = 0; gy < kGridRows; ++gy)
        for (int gx = 0; gx < kGridColumns; ++gx)
            fillRect(img, originX + gx * cell, originY + gy * cell, cell, cell,
                     cells[gy * kGridColumns + gx] ? kBlackY : kWhiteY,
                     kNeutralC, kNeutralC);
}

}  // namespace testsignal
