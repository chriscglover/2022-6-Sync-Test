#include "pcapreplay/crc.h"

namespace pcapreplay {

namespace {

// Ten-bit-wide CRC table plus a reversal table.
//
// The register shifts left (message bits enter at the top) but SDI feeds each
// word LSB first, so the word is bit-reversed before it indexes the table --
// that reversal is precomputed too.
struct Crc18Tables {
    std::uint32_t step[1024];     // 10-bit-at-a-time state transition
    std::uint16_t rev10[1024];    // bit-reversed 10-bit words

    Crc18Tables() {
        for (std::uint32_t i = 0; i < 1024; ++i) {
            std::uint16_t r = 0;
            for (int b = 0; b < 10; ++b)
                if (i & (1u << b)) r = std::uint16_t(r | (1u << (9 - b)));
            rev10[i] = r;

            // Remainder of the 10-bit value i, fed MSB-first into a zeroed
            // register using the same recurrence as the bitwise form.
            std::uint32_t crc = 0;
            for (int b = 9; b >= 0; --b) {
                const std::uint32_t in  = (i >> b) & 1u;
                const std::uint32_t top = (crc >> 17) & 1u;
                crc = (crc << 1) & kCrc18Mask;
                if (in ^ top) crc ^= kCrc18Poly;
            }
            step[i] = crc;
        }
    }
};

const Crc18Tables& tables() {
    static const Crc18Tables t;
    return t;
}

inline std::uint32_t crcStep(std::uint32_t crc, std::uint32_t word,
                             const Crc18Tables& t) {
    const std::uint32_t idx = ((crc >> 8) ^ t.rev10[word & 0x3FFu]) & 0x3FFu;
    return ((crc << 10) ^ t.step[idx]) & kCrc18Mask;
}

}  // namespace

std::uint32_t crc18Bitwise(const std::uint16_t* words, std::size_t count,
                           std::uint32_t crc) {
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t w = words[i] & 0x3FFu;
        for (int b = 0; b < 10; ++b) {                 // LSB first
            const std::uint32_t in  = (w >> b) & 1u;
            const std::uint32_t top = (crc >> 17) & 1u;
            crc = (crc << 1) & kCrc18Mask;
            if (in ^ top) crc ^= kCrc18Poly;
        }
    }
    return crc & kCrc18Mask;
}

std::uint32_t crc18(const std::uint16_t* words, std::size_t count,
                    std::uint32_t crc) {
    const Crc18Tables& t = tables();
    for (std::size_t i = 0; i < count; ++i)
        crc = crcStep(crc, words[i], t);
    return crc;
}

std::uint32_t crc18Strided(const std::uint16_t* words, std::size_t count,
                           std::size_t stride, std::uint32_t crc) {
    const Crc18Tables& t = tables();
    for (std::size_t i = 0; i < count; ++i)
        crc = crcStep(crc, words[i * stride], t);
    return crc;
}

Crc18Words crc18ToWords(std::uint32_t crc) {
    const std::uint32_t r = reflect18(crc & kCrc18Mask);
    Crc18Words w{};
    w.crc0 = withNotB8(std::uint16_t(r & 0x1FFu));
    w.crc1 = withNotB8(std::uint16_t((r >> 9) & 0x1FFu));
    return w;
}

std::uint32_t crc18FromWords(std::uint16_t crc0, std::uint16_t crc1) {
    const std::uint32_t r = ((std::uint32_t(crc1) & 0x1FFu) << 9) |
                             (std::uint32_t(crc0) & 0x1FFu);
    return reflect18(r);
}

std::uint16_t ancChecksum(const std::uint16_t* fromDid, std::size_t count) {
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < count; ++i)
        sum += fromDid[i] & 0x1FFu;      // 9-bit sum, carries discarded
    return withNotB8(std::uint16_t(sum & 0x1FFu));
}

}  // namespace pcapreplay
