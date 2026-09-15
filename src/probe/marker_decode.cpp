#include "probe/marker_decode.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "overlay.h"

namespace testsignal {

namespace {

constexpr int kColumns = 36;
constexpr int kRows = 22;
constexpr int kDataColumns = 32;
constexpr int kDataRows = 18;
constexpr int kDataX = 2;
constexpr int kDataY = 2;
constexpr int kThresholdY = 128;

}  // namespace

MarkerBox nativeMarkerBox(int width, int height) {
    const int cell = std::min(width / 320, height / 180);
    if (cell < 1) return {};
    return {2 * cell, height - (kRows + 2) * cell, kColumns * cell, kRows * cell};
}

MarkerBox scaledMarkerBox(int sourceW, int sourceH, int rx, int ry, int rw, int rh) {
    const MarkerBox native = nativeMarkerBox(sourceW, sourceH);
    if (!native.valid() || rw < 1 || rh < 1) return {};
    const double sx = double(rw) / sourceW;
    const double sy = double(rh) / sourceH;
    MarkerBox b{rx + int(std::lround(native.x * sx)), ry + int(std::lround(native.y * sy)),
                int(std::lround(native.w * sx)), int(std::lround(native.h * sy))};
    return b.valid() ? b : MarkerBox{};
}

MarkerRead decodeMarker(const std::uint8_t* luma, int width, int height, MarkerBox box) {
    MarkerRead out;
    if (!box.valid() || box.x < 0 || box.y < 0 || box.x + box.w > width || box.y + box.h > height)
        return out;

    std::array<bool, kColumns * kRows> black{};
    for (int gy = 0; gy < kRows; ++gy) {
        const int y = box.y + ((2 * gy + 1) * box.h) / (2 * kRows);
        for (int gx = 0; gx < kColumns; ++gx) {
            const int x = box.x + ((2 * gx + 1) * box.w) / (2 * kColumns);
            black[std::size_t(gy * kColumns + gx)] = luma[std::size_t(y) * std::size_t(width) + std::size_t(x)] < kThresholdY;
        }
    }

    // Finder border: top row and left column black, bottom row and right
    // column alternating.
    int expected = 0, matched = 0;
    auto expect = [&](int x, int y, bool isBlack) {
        ++expected;
        if (black[std::size_t(y * kColumns + x)] == isBlack) ++matched;
    };
    for (int x = 0; x < kColumns; ++x) expect(x, 0, true);
    for (int y = 1; y < kRows; ++y) expect(0, y, true);
    for (int x = 1; x < kColumns; ++x) expect(x, kRows - 1, x % 2 == 0);
    for (int y = 1; y < kRows - 1; ++y) expect(kColumns - 1, y, y % 2 == 0);
    if (matched < expected * 9 / 10) return out;

    std::array<std::uint8_t, 24> payload{};
    for (int bit = 0; bit < 24 * 8; ++bit) {
        int votes = 0;
        for (int copy = 0; copy < 3; ++copy) {
            const int index = bit * 3 + copy;
            const int x = kDataX + index % kDataColumns;
            const int y = kDataY + index / kDataColumns;
            votes += black[std::size_t(y * kColumns + x)] ? 1 : 0;
        }
        if (votes >= 2) payload[std::size_t(bit / 8)] |= std::uint8_t(1 << (7 - bit % 8));
    }
    if (payload[0] != 'M' || payload[1] != 'V' || payload[2] != 1) return out;

    std::uint64_t runTag = 0, ms = 0;
    std::uint32_t frame = 0;
    for (int i = 0; i < 8; ++i) runTag = (runTag << 8) | payload[std::size_t(4 + i)];
    for (int i = 0; i < 4; ++i) frame = (frame << 8) | payload[std::size_t(12 + i)];
    for (int i = 0; i < 6; ++i) ms = (ms << 8) | payload[std::size_t(16 + i)];
    // Re-encoding reproduces the CRC; a payload that does not is rejected.
    if (markerPayload(runTag, frame, ms) != payload) return out;

    out.ok = true;
    out.runTag = runTag;
    out.frameNumber = frame;
    out.captureUtcMs = ms;
    return out;
}

}  // namespace testsignal
