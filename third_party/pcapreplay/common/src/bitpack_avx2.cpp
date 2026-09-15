// AVX2 / 64-bit fast paths. Compiled with /arch:AVX2 in its own TU; only ever
// entered via the runtime probe in bitpack.cpp.
//
// The 8-bit <-> 10-bit widening really does vectorise cleanly, so those use
// AVX2 proper. The 10-bit packer does not: 4 words -> 5 bytes is a ratio that
// fights every shuffle you can build, and the 64-bit assemble-and-bswap form
// below beats a shuffle-based AVX2 version in practice while being far easier
// to prove correct. Both are validated against the scalar reference by
// tests/test_bitpack.cpp.

#include "pcapreplay/bitpack.h"

#include <immintrin.h>
#ifdef _MSC_VER
#include <intrin.h>
#else
// The same instruction, a different spelling.
#define _byteswap_uint64(x) __builtin_bswap64(x)
#endif

#include <cstring>

namespace pcapreplay::avx2 {

void pack10(const std::uint16_t* src, std::size_t count, std::uint8_t* dst) {
    std::size_t i = 0;

    // Overlapping 8-byte stores: we emit 5 useful bytes and let the next
    // iteration overwrite the 3 bytes of slack. The final group must therefore
    // use the exact path so we never write past the buffer.
    const std::size_t bulk = (count >= 4) ? count - 4 : 0;
    for (; i < bulk; i += 4) {
        const std::uint64_t w0 = src[i + 0] & 0x3FFu;
        const std::uint64_t w1 = src[i + 1] & 0x3FFu;
        const std::uint64_t w2 = src[i + 2] & 0x3FFu;
        const std::uint64_t w3 = src[i + 3] & 0x3FFu;

        const std::uint64_t v = (w0 << 30) | (w1 << 20) | (w2 << 10) | w3;
        const std::uint64_t be = _byteswap_uint64(v << 24);
        std::memcpy(dst, &be, sizeof be);
        dst += 5;
    }

    for (; i + 4 <= count; i += 4) {
        const std::uint32_t w0 = src[i + 0] & 0x3FFu;
        const std::uint32_t w1 = src[i + 1] & 0x3FFu;
        const std::uint32_t w2 = src[i + 2] & 0x3FFu;
        const std::uint32_t w3 = src[i + 3] & 0x3FFu;
        *dst++ = std::uint8_t(w0 >> 2);
        *dst++ = std::uint8_t(((w0 & 0x003u) << 6) | (w1 >> 4));
        *dst++ = std::uint8_t(((w1 & 0x00Fu) << 4) | (w2 >> 6));
        *dst++ = std::uint8_t(((w2 & 0x03Fu) << 2) | (w3 >> 8));
        *dst++ = std::uint8_t(w3 & 0x0FFu);
    }
}

void unpack10(const std::uint8_t* src, std::size_t count, std::uint16_t* dst) {
    std::size_t i = 0;

    // Symmetrically, the bulk path over-reads by 3 bytes.
    const std::size_t bulk = (count >= 4) ? count - 4 : 0;
    for (; i < bulk; i += 4) {
        std::uint64_t be;
        std::memcpy(&be, src, sizeof be);
        const std::uint64_t v = _byteswap_uint64(be) >> 24;   // 40 significant bits
        src += 5;
        dst[i + 0] = std::uint16_t((v >> 30) & 0x3FFu);
        dst[i + 1] = std::uint16_t((v >> 20) & 0x3FFu);
        dst[i + 2] = std::uint16_t((v >> 10) & 0x3FFu);
        dst[i + 3] = std::uint16_t(v & 0x3FFu);
    }

    for (; i + 4 <= count; i += 4) {
        const std::uint32_t b0 = src[0], b1 = src[1], b2 = src[2],
                            b3 = src[3], b4 = src[4];
        src += 5;
        dst[i + 0] = std::uint16_t((b0 << 2) | (b1 >> 6));
        dst[i + 1] = std::uint16_t(((b1 & 0x3Fu) << 4) | (b2 >> 4));
        dst[i + 2] = std::uint16_t(((b2 & 0x0Fu) << 6) | (b3 >> 2));
        dst[i + 3] = std::uint16_t(((b3 & 0x03u) << 8) | b4);
    }
}

void uyvy8ToWords(const std::uint8_t* src, std::size_t pixels, std::uint16_t* dst) {
    const std::size_t words = pixels * 2;
    std::size_t i = 0;

    for (; i + 16 <= words; i += 16) {
        const __m128i b  = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i));
        const __m256i w  = _mm256_cvtepu8_epi16(b);
        const __m256i s  = _mm256_slli_epi16(w, 2);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), s);
    }
    for (; i < words; ++i)
        dst[i] = std::uint16_t(std::uint16_t(src[i]) << 2);
}

void wordsToUyvy8(const std::uint16_t* src, std::size_t pixels, std::uint8_t* dst) {
    const std::size_t words = pixels * 2;
    std::size_t i = 0;

    const __m256i mask = _mm256_set1_epi16(0x3FF);
    for (; i + 32 <= words; i += 32) {
        __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + i));
        __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + i + 16));
        a = _mm256_srli_epi16(_mm256_and_si256(a, mask), 2);
        b = _mm256_srli_epi16(_mm256_and_si256(b, mask), 2);
        // packus interleaves the 128-bit lanes, so undo that with permute4x64.
        __m256i p = _mm256_packus_epi16(a, b);
        p = _mm256_permute4x64_epi64(p, _MM_SHUFFLE(3, 1, 2, 0));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), p);
    }
    for (; i < words; ++i)
        dst[i] = std::uint8_t((src[i] & 0x3FFu) >> 2);
}

}  // namespace pcapreplay::avx2
