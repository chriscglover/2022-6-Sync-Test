// SMPTE ST 292 line CRC and ST 291 ancillary-packet checksum.
//
// The CRC is an 18-bit LFSR with polynomial x^18 + x^5 + x^4 + 1, run LSB-first
// over the 10-bit words of a line, covering the active picture plus the EAV and
// line-number words. It is computed independently for the luma and chroma
// streams and the two results land in that stream's CRC0/CRC1 words.
//
// The polynomial taps and the exact coverage span are the two details here most
// worth confirming against real SDI kit -- see
// docs/07-validation-and-open-questions.md. Sender and receiver share this code,
// so a loopback test proves self-consistency but not conformance.
#pragma once

#include <cstddef>
#include <cstdint>

namespace pcapreplay {

// Feedback taps below x^18: x^5 + x^4 + 1.
inline constexpr std::uint32_t kCrc18Poly = 0x31u;
inline constexpr std::uint32_t kCrc18Mask = 0x3FFFFu;

// Run the CRC over `count` 10-bit words, continuing from `crc` (0 to start).
//
// Table-driven: one lookup per 10-bit word rather than ten LFSR steps. The
// bitwise form cost 50.7 ms per 1080p50 frame -- 19.7 fps, which was the hard
// ceiling on the sender's capture thread. crc18Bitwise() is kept as the
// reference the table is validated against.
std::uint32_t crc18(const std::uint16_t* words, std::size_t count,
                    std::uint32_t crc = 0);

// Reference implementation. Correct, obvious, and far too slow for line rate.
std::uint32_t crc18Bitwise(const std::uint16_t* words, std::size_t count,
                           std::uint32_t crc = 0);

// Same, but walking every `stride`-th word. HD multiplexes Cb/Y/Cr/Y, so the
// luma and chroma CRCs are two stride-2 passes over the line rather than two
// gathered copies of it.
std::uint32_t crc18Strided(const std::uint16_t* words, std::size_t count,
                           std::size_t stride, std::uint32_t crc = 0);

// Bit-reverse an 18-bit value.
//
// The register is reflected before it is split into the carrier words. This is
// not a guess: a brute-force solve against a real third-party 1080i25 stream
// (docs/07-validation-and-open-questions.md) confirmed polynomial 0x31,
// LSB-first, no xor-out, coverage active..LN -- with reflected output as the
// one and only difference from the obvious reading. Getting this wrong makes a
// conformant receiver flag every line as errored.
inline constexpr std::uint32_t reflect18(std::uint32_t v) {
    std::uint32_t r = 0;
    for (int i = 0; i < 18; ++i)
        if (v & (1u << i)) r |= 1u << (17 - i);
    return r;
}

// Split an 18-bit CRC into the two 10-bit words that carry it.
// Each word holds 9 bits of the *reflected* CRC in bits 8..0, with bit 9 as the
// inverse of bit 8.
struct Crc18Words {
    std::uint16_t crc0;   // CRC bits 8..0
    std::uint16_t crc1;   // CRC bits 17..9
};
Crc18Words crc18ToWords(std::uint32_t crc);
std::uint32_t crc18FromWords(std::uint16_t crc0, std::uint16_t crc1);

// ST 291 ancillary checksum: 9-bit sum of every word from DID through the last
// user word, returned as a 10-bit word with bit 9 = !bit 8.
std::uint16_t ancChecksum(const std::uint16_t* fromDid, std::size_t count);

// Set bit 9 to the inverse of bit 8, the "not b8" convention shared by the
// line-number, CRC and ancillary words.
inline std::uint16_t withNotB8(std::uint16_t nineBits) {
    const std::uint16_t v = nineBits & 0x1FFu;
    return std::uint16_t(v | (((~v) & 0x100u) << 1));
}

}  // namespace pcapreplay
