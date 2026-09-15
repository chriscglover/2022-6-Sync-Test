// RTP and the ST 2022-6 HBRMT payload header.
//
// HBRMT header layout, confirmed against the ST 2022-6 field masks (see
// docs/02-st2022-6-wire-format.md for the derivation). Fields are nibble-packed
// and several straddle byte boundaries, so this is all done by hand:
//
//   byte 0 : Ext[7:4]  F[3]  VSID[2:0]
//   byte 1 : FRCount[7:0]
//   byte 2 : R[7:6]  S[5:4]  FEC[3:1]  CF[3]
//   byte 3 : CF[2:0] << 5   reserved[4:0]
//   byte 4 : MAP[7:4]    FRAME[7:4]
//   byte 5 : FRAME[3:0]  FRATE[7:4]
//   byte 6 : FRATE[3:0]  SAMPLE[3:0]
//   byte 7 : reserved
//   byte 8 : video timestamp, 4 bytes, present only when CF != 0
//
// One media datagram carries exactly 1376 payload bytes, so a full packet is
// 12 (RTP) + 12 (HBRMT) + 1376 = 1400 bytes of UDP payload, or 1428 on the wire
// with IP and UDP headers -- comfortably inside a 1500-byte MTU.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "pcapreplay/sdi_format.h"

namespace pcapreplay {

inline constexpr std::size_t kHbrmtPayloadBytes = 1376;
inline constexpr std::size_t kRtpHeaderBytes    = 12;
inline constexpr std::size_t kHbrmtHeaderBytes  = 8;
inline constexpr std::size_t kHbrmtTimestampBytes = 4;
inline constexpr std::size_t kDatagramBytes =
    kRtpHeaderBytes + kHbrmtHeaderBytes + kHbrmtTimestampBytes + kHbrmtPayloadBytes;

// Default dynamic payload type for ST 2022-6. Confirmed against a real stream.
inline constexpr std::uint8_t kDefaultPayloadType = 98;

// The RTP header timestamp runs at 27 MHz -- NOT at the HBRMT CF rate, which
// describes the separate video timestamp inside the payload header. Measured
// from a real 1080i25 stream: the RTP timestamp is constant across every packet
// of a frame and steps by exactly 1,080,000 = 27 MHz / 25 between frames.
inline constexpr std::uint32_t kRtpClockHz = 27000000;

// Ticks of the 27 MHz RTP clock per video frame, rounded to nearest.
// 1080i25 -> 1080000, 1080p50 -> 540000, 1080i29.97 -> 900900.
constexpr std::uint32_t rtpTicksPerFrame(int frameRateNum, int frameRateDen) {
    if (frameRateNum <= 0) return 0;
    const std::uint64_t n = std::uint64_t(kRtpClockHz) * std::uint64_t(frameRateDen);
    return std::uint32_t((n + std::uint64_t(frameRateNum) / 2) /
                         std::uint64_t(frameRateNum));
}

// ---- RTP -------------------------------------------------------------------

struct RtpHeader {
    std::uint8_t  version       = 2;
    bool          padding       = false;
    bool          extension     = false;
    std::uint8_t  csrcCount     = 0;
    bool          marker        = false;
    std::uint8_t  payloadType   = kDefaultPayloadType;
    std::uint16_t sequence      = 0;
    std::uint32_t timestamp     = 0;
    std::uint32_t ssrc          = 0;
};

void writeRtpHeader(const RtpHeader& h, std::uint8_t* dst);
bool readRtpHeader(const std::uint8_t* src, std::size_t len, RtpHeader& out);

// ---- HBRMT -----------------------------------------------------------------

struct HbrmtHeader {
    std::uint8_t  ext        = 0;
    bool          formatFlag = true;   // F: FRAME/FRATE/SAMPLE are meaningful
    std::uint8_t  vsid       = 0;      // 0 = primary stream
    std::uint8_t  frCount    = 0;      // frame counter, wraps at 256
    std::uint8_t  r          = 0;      // timestamp reference: 0 = not locked
    std::uint8_t  s          = 0;      // scrambling: 0 = none
    std::uint8_t  fec        = 0;      // 0 = no ST 2022-5 FEC
    std::uint8_t  cf         = 0;      // clock frequency of videoTimestamp
    std::uint8_t  map        = 0;      // 0 = direct sample mapping
    std::uint8_t  frame      = 0;      // FRAME code
    std::uint8_t  frate      = 0;      // FRATE code
    std::uint8_t  sample     = 0;      // SAMPLE code
    std::uint32_t videoTimestamp = 0;  // present iff cf != 0

    std::size_t sizeBytes() const {
        return kHbrmtHeaderBytes + (cf ? kHbrmtTimestampBytes : 0);
    }
};

// Populate the format-describing fields from the SDI format table.
HbrmtHeader hbrmtForFormat(const SdiFormatInfo& fi);

void        writeHbrmtHeader(const HbrmtHeader& h, std::uint8_t* dst);
bool        readHbrmtHeader(const std::uint8_t* src, std::size_t len, HbrmtHeader& out);

// Convenience: assemble a complete datagram. `payload` must be exactly
// kHbrmtPayloadBytes. Returns bytes written.
std::size_t buildDatagram(const RtpHeader& rtp, const HbrmtHeader& hbrmt,
                          std::span<const std::uint8_t> payload,
                          std::uint8_t* dst, std::size_t dstCapacity);

// Split a received datagram into its headers and payload.
struct ParsedDatagram {
    RtpHeader                  rtp;
    HbrmtHeader                hbrmt;
    std::span<const std::uint8_t> payload;
};
bool parseDatagram(std::span<const std::uint8_t> packet, ParsedDatagram& out);

}  // namespace pcapreplay
