#include "pcapreplay/bitpack.h"

#ifdef _MSC_VER
#include <intrin.h>
#else
#include <cpuid.h>
#include <immintrin.h>
#endif

namespace pcapreplay {

namespace scalar {

void pack10(const std::uint16_t* src, std::size_t count, std::uint8_t* dst) {
    for (std::size_t i = 0; i + 4 <= count; i += 4) {
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
    for (std::size_t i = 0; i + 4 <= count; i += 4) {
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
    for (std::size_t i = 0; i < words; ++i)
        dst[i] = std::uint16_t(std::uint16_t(src[i]) << 2);
}

void wordsToUyvy8(const std::uint16_t* src, std::size_t pixels, std::uint8_t* dst) {
    const std::size_t words = pixels * 2;
    for (std::size_t i = 0; i < words; ++i)
        dst[i] = std::uint8_t((src[i] & 0x3FFu) >> 2);
}

}  // namespace scalar

// ---------------------------------------------------------------------------

namespace {

// CPUID and XGETBV, which MSVC and GCC spell differently. Wrapped rather than
// #ifdef-ed at the call site so the probe below reads as the sequence of checks
// it is.
inline void cpuid(int leaf, int sub, unsigned out[4]) {
#ifdef _MSC_VER
    __cpuidex(reinterpret_cast<int*>(out), leaf, sub);
#else
    __cpuid_count(unsigned(leaf), unsigned(sub), out[0], out[1], out[2], out[3]);
#endif
}

// Only reached once CPUID has said OSXSAVE is set, so the instruction is
// guaranteed to be legal here. Issued directly on GCC because _xgetbv is only
// declared when the compiler is itself targeting a machine with XSAVE, and this
// translation unit is deliberately built for baseline x86-64.
inline unsigned long long xcr0() {
#ifdef _MSC_VER
    return _xgetbv(0);
#else
    unsigned int lo = 0, hi = 0;
    __asm__ __volatile__("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return (static_cast<unsigned long long>(hi) << 32) | lo;
#endif
}

}  // namespace

bool avx2Available() {
    static const bool has = [] {
        unsigned info[4]{};
        cpuid(0, 0, info);
        if (info[0] < 7) return false;

        cpuid(7, 0, info);
        const bool avx2 = (info[1] & (1u << 5)) != 0;     // EBX bit 5

        cpuid(1, 0, info);
        const bool osxsave = (info[2] & (1u << 27)) != 0;
        const bool avx     = (info[2] & (1u << 28)) != 0;
        if (!(avx2 && avx && osxsave)) return false;

        // Confirm the OS actually saves YMM state. A CPU that has AVX2 under a
        // kernel that does not preserve the upper halves would fault, or worse,
        // silently corrupt them across a context switch.
        return (xcr0() & 0x6) == 0x6;
    }();
    return has;
}

void pack10(const std::uint16_t* src, std::size_t count, std::uint8_t* dst) {
    if (avx2Available()) avx2::pack10(src, count, dst);
    else                 scalar::pack10(src, count, dst);
}

void unpack10(const std::uint8_t* src, std::size_t count, std::uint16_t* dst) {
    if (avx2Available()) avx2::unpack10(src, count, dst);
    else                 scalar::unpack10(src, count, dst);
}

void uyvy8ToWords(const std::uint8_t* src, std::size_t pixels, std::uint16_t* dst) {
    if (avx2Available()) avx2::uyvy8ToWords(src, pixels, dst);
    else                 scalar::uyvy8ToWords(src, pixels, dst);
}

void wordsToUyvy8(const std::uint16_t* src, std::size_t pixels, std::uint8_t* dst) {
    if (avx2Available()) avx2::wordsToUyvy8(src, pixels, dst);
    else                 scalar::wordsToUyvy8(src, pixels, dst);
}

// P216 is planar, so the gather pattern differs enough that the scalar version
// is the only one; it is not on the same hot path as the packer.
void p216RowToWords(const std::uint16_t* y, const std::uint16_t* cbcr,
                    std::size_t pixels, std::uint16_t* dst) {
    for (std::size_t p = 0, w = 0; p < pixels; p += 2) {
        dst[w++] = std::uint16_t(cbcr[p + 0] >> 6);   // Cb
        dst[w++] = std::uint16_t(y[p + 0] >> 6);      // Y0
        dst[w++] = std::uint16_t(cbcr[p + 1] >> 6);   // Cr
        dst[w++] = std::uint16_t(y[p + 1] >> 6);      // Y1
    }
}

void wordsToP216Row(const std::uint16_t* src, std::size_t pixels,
                    std::uint16_t* y, std::uint16_t* cbcr) {
    for (std::size_t p = 0, w = 0; p < pixels; p += 2) {
        cbcr[p + 0] = std::uint16_t((src[w + 0] & 0x3FFu) << 6);
        y[p + 0]    = std::uint16_t((src[w + 1] & 0x3FFu) << 6);
        cbcr[p + 1] = std::uint16_t((src[w + 2] & 0x3FFu) << 6);
        y[p + 1]    = std::uint16_t((src[w + 3] & 0x3FFu) << 6);
        w += 4;
    }
}

}  // namespace pcapreplay
