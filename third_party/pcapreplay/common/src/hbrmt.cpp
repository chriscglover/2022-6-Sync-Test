#include "pcapreplay/hbrmt.h"

#include <cstring>

namespace pcapreplay {
namespace {

inline void put32be(std::uint8_t* p, std::uint32_t v) {
    p[0] = std::uint8_t(v >> 24);
    p[1] = std::uint8_t(v >> 16);
    p[2] = std::uint8_t(v >> 8);
    p[3] = std::uint8_t(v);
}
inline std::uint32_t get32be(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8)  |  std::uint32_t(p[3]);
}

}  // namespace

// ---- RTP -------------------------------------------------------------------

void writeRtpHeader(const RtpHeader& h, std::uint8_t* dst) {
    dst[0] = std::uint8_t(((h.version & 0x03) << 6) |
                          (h.padding   ? 0x20 : 0) |
                          (h.extension ? 0x10 : 0) |
                          (h.csrcCount & 0x0F));
    dst[1] = std::uint8_t((h.marker ? 0x80 : 0) | (h.payloadType & 0x7F));
    dst[2] = std::uint8_t(h.sequence >> 8);
    dst[3] = std::uint8_t(h.sequence);
    put32be(dst + 4, h.timestamp);
    put32be(dst + 8, h.ssrc);
}

bool readRtpHeader(const std::uint8_t* src, std::size_t len, RtpHeader& out) {
    if (len < kRtpHeaderBytes) return false;
    out.version     = std::uint8_t((src[0] >> 6) & 0x03);
    out.padding     = (src[0] & 0x20) != 0;
    out.extension   = (src[0] & 0x10) != 0;
    out.csrcCount   = std::uint8_t(src[0] & 0x0F);
    out.marker      = (src[1] & 0x80) != 0;
    out.payloadType = std::uint8_t(src[1] & 0x7F);
    out.sequence    = std::uint16_t((std::uint16_t(src[2]) << 8) | src[3]);
    out.timestamp   = get32be(src + 4);
    out.ssrc        = get32be(src + 8);
    return out.version == 2;
}

// ---- HBRMT -----------------------------------------------------------------

HbrmtHeader hbrmtForFormat(const SdiFormatInfo& fi) {
    HbrmtHeader h;
    h.formatFlag = true;
    h.vsid   = 0;      // both -7 paths are the primary stream; they are
                       // byte-identical copies, not a protect stream
    h.map    = 0;      // direct sample mapping
    h.frame  = fi.frameCode;
    h.frate  = fi.frateCode;
    h.sample = fi.sampleCode;
    h.cf     = fi.cfCode;
    return h;
}

void writeHbrmtHeader(const HbrmtHeader& h, std::uint8_t* dst) {
    dst[0] = std::uint8_t(((h.ext & 0x0F) << 4) |
                          (h.formatFlag ? 0x08 : 0x00) |
                          (h.vsid & 0x07));
    dst[1] = h.frCount;
    dst[2] = std::uint8_t(((h.r   & 0x03) << 6) |
                          ((h.s   & 0x03) << 4) |
                          ((h.fec & 0x07) << 1) |
                          ((h.cf  >> 3)   & 0x01));
    dst[3] = std::uint8_t((h.cf & 0x07) << 5);
    dst[4] = std::uint8_t(((h.map   & 0x0F) << 4) | ((h.frame >> 4) & 0x0F));
    dst[5] = std::uint8_t(((h.frame & 0x0F) << 4) | ((h.frate >> 4) & 0x0F));
    dst[6] = std::uint8_t(((h.frate & 0x0F) << 4) |  (h.sample & 0x0F));
    dst[7] = 0;
    if (h.cf) put32be(dst + 8, h.videoTimestamp);
}

bool readHbrmtHeader(const std::uint8_t* src, std::size_t len, HbrmtHeader& out) {
    if (len < kHbrmtHeaderBytes) return false;

    out.ext        = std::uint8_t((src[0] >> 4) & 0x0F);
    out.formatFlag = (src[0] & 0x08) != 0;
    out.vsid       = std::uint8_t(src[0] & 0x07);
    out.frCount    = src[1];
    out.r          = std::uint8_t((src[2] >> 6) & 0x03);
    out.s          = std::uint8_t((src[2] >> 4) & 0x03);
    out.fec        = std::uint8_t((src[2] >> 1) & 0x07);
    out.cf         = std::uint8_t(((src[2] & 0x01) << 3) | ((src[3] >> 5) & 0x07));
    out.map        = std::uint8_t((src[4] >> 4) & 0x0F);
    out.frame      = std::uint8_t(((src[4] & 0x0F) << 4) | ((src[5] >> 4) & 0x0F));
    out.frate      = std::uint8_t(((src[5] & 0x0F) << 4) | ((src[6] >> 4) & 0x0F));
    out.sample     = std::uint8_t(src[6] & 0x0F);

    if (out.cf) {
        if (len < kHbrmtHeaderBytes + kHbrmtTimestampBytes) return false;
        out.videoTimestamp = get32be(src + 8);
    } else {
        out.videoTimestamp = 0;
    }
    return true;
}

// ---- Whole datagrams -------------------------------------------------------

std::size_t buildDatagram(const RtpHeader& rtp, const HbrmtHeader& hbrmt,
                          std::span<const std::uint8_t> payload,
                          std::uint8_t* dst, std::size_t dstCapacity) {
    const std::size_t total = kRtpHeaderBytes + hbrmt.sizeBytes() + payload.size();
    if (dstCapacity < total) return 0;

    writeRtpHeader(rtp, dst);
    writeHbrmtHeader(hbrmt, dst + kRtpHeaderBytes);
    std::memcpy(dst + kRtpHeaderBytes + hbrmt.sizeBytes(),
                payload.data(), payload.size());
    return total;
}

bool parseDatagram(std::span<const std::uint8_t> packet, ParsedDatagram& out) {
    if (!readRtpHeader(packet.data(), packet.size(), out.rtp)) return false;

    const std::size_t rtpLen = kRtpHeaderBytes + std::size_t(out.rtp.csrcCount) * 4;
    if (packet.size() < rtpLen) return false;

    if (!readHbrmtHeader(packet.data() + rtpLen, packet.size() - rtpLen, out.hbrmt))
        return false;

    const std::size_t off = rtpLen + out.hbrmt.sizeBytes();
    if (packet.size() < off) return false;
    out.payload = packet.subspan(off);
    return true;
}

}  // namespace pcapreplay
