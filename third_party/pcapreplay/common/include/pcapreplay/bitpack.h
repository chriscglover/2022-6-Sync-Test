// Packing between 10-bit SDI words and the byte stream ST 2022-6 puts on the
// wire, plus the pixel-format conversions to and from what a frame source
// hands us.
//
// The wire packing is big-endian and continuous: four 10-bit words occupy
// exactly five bytes, MSB first.
//
//   byte0 = w0[9:2]
//   byte1 = w0[1:0] << 6 | w1[9:4]
//   byte2 = w1[3:0] << 4 | w2[9:6]
//   byte3 = w2[5:0] << 2 | w3[9:8]
//   byte4 = w3[7:0]
//
// Every format in the table has an even totalSamples, so a line is always a
// whole number of 4-word groups and no group ever straddles a line boundary.
#pragma once

#include <cstddef>
#include <cstdint>

namespace pcapreplay {

// ---- 10-bit word stream <-> packed wire bytes -----------------------------

// Pack `count` 10-bit words (low 10 bits significant) into `dst`.
// `count` must be a multiple of 4; dst must have room for count/4*5 bytes.
void pack10(const std::uint16_t* src, std::size_t count, std::uint8_t* dst);

// Inverse of pack10. `count` must be a multiple of 4.
void unpack10(const std::uint8_t* src, std::size_t count, std::uint16_t* dst);

inline std::size_t packedBytes(std::size_t words) { return words / 4 * 5; }

// ---- Packed pixel formats -> 10-bit 4:2:2 mux -----------------------------
//
// SDI multiplexes as Cb Y0 Cr Y1, which is exactly UYVY's byte order, so these
// are widenings rather than reorderings.

// UYVY 8-bit -> 10-bit words, scaling by <<2. `pixels` is the pixel count for
// the row (must be even); writes pixels*2 words.
void uyvy8ToWords(const std::uint8_t* src, std::size_t pixels, std::uint16_t* dst);

// P216 (planar 16-bit Y + interleaved 16-bit CbCr) -> 10-bit words, >>6.
// Reads `pixels` luma samples and pixels/2 chroma pairs for one row.
void p216RowToWords(const std::uint16_t* y, const std::uint16_t* cbcr,
                    std::size_t pixels, std::uint16_t* dst);

// ---- 10-bit mux -> packed pixel formats -----------------------------------

void wordsToUyvy8(const std::uint16_t* src, std::size_t pixels, std::uint8_t* dst);
void wordsToP216Row(const std::uint16_t* src, std::size_t pixels,
                    std::uint16_t* y, std::uint16_t* cbcr);

// ---- Runtime dispatch ------------------------------------------------------

bool avx2Available();

// The AVX2 implementations, defined in bitpack_avx2.cpp and only called when
// avx2Available(). Exposed so the tests can compare them against scalar.
namespace avx2 {
void pack10(const std::uint16_t* src, std::size_t count, std::uint8_t* dst);
void unpack10(const std::uint8_t* src, std::size_t count, std::uint16_t* dst);
void uyvy8ToWords(const std::uint8_t* src, std::size_t pixels, std::uint16_t* dst);
void wordsToUyvy8(const std::uint16_t* src, std::size_t pixels, std::uint8_t* dst);
}  // namespace avx2

namespace scalar {
void pack10(const std::uint16_t* src, std::size_t count, std::uint8_t* dst);
void unpack10(const std::uint8_t* src, std::size_t count, std::uint16_t* dst);
void uyvy8ToWords(const std::uint8_t* src, std::size_t pixels, std::uint16_t* dst);
void wordsToUyvy8(const std::uint16_t* src, std::size_t pixels, std::uint8_t* dst);
}  // namespace scalar

}  // namespace pcapreplay
