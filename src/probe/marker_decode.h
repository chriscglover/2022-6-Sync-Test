// Reading the frame marker back out of a received picture, at its native
// position or wherever a multiviewer or scaler has placed the source.
//
// Sample each grid cell at its centre, require 90% of the finder border,
// majority-vote the three copies of each payload bit, and accept only a
// payload whose CRC checks.
#pragma once

#include <cstdint>

namespace testsignal {

struct MarkerBox {
    int x = 0, y = 0, w = 0, h = 0;
    bool valid() const { return w >= 36 && h >= 22; }
};

struct MarkerRead {
    bool          ok = false;
    std::uint64_t runTag = 0;
    std::uint32_t frameNumber = 0;
    std::uint64_t captureUtcMs = 0;
};

// Where the sender draws the marker on a raster of this size.
MarkerBox nativeMarkerBox(int width, int height);

// Where that marker lands once a `sourceW` x `sourceH` picture has been scaled
// into the rectangle (rx, ry, rw, rh) of a larger frame.
MarkerBox scaledMarkerBox(int sourceW, int sourceH, int rx, int ry, int rw, int rh);

MarkerRead decodeMarker(const std::uint8_t* luma, int width, int height, MarkerBox box);

}  // namespace testsignal
