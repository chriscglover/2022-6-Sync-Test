#include "pcapreplay/nmos/uuid.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

namespace pcapreplay::nmos {
namespace {

// ---- SHA-1 ------------------------------------------------------------------
// Only here because UUID v5 is defined in terms of it. Not used for anything
// security-bearing.

struct Sha1 {
    std::uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu,
                          0x10325476u, 0xC3D2E1F0u};
    std::uint64_t total = 0;
    std::uint8_t  block[64] = {};
    std::size_t   fill = 0;

    static std::uint32_t rol(std::uint32_t v, int n) {
        return (v << n) | (v >> (32 - n));
    }

    void compress(const std::uint8_t* p) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (std::uint32_t(p[i * 4]) << 24) | (std::uint32_t(p[i * 4 + 1]) << 16) |
                   (std::uint32_t(p[i * 4 + 2]) << 8) | std::uint32_t(p[i * 4 + 3]);
        for (int i = 16; i < 80; ++i)
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);            k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d;                     k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d);   k = 0x8F1BBCDCu; }
            else             { f = b ^ c ^ d;                     k = 0xCA62C1D6u; }
            const std::uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }

    void update(const std::uint8_t* p, std::size_t n) {
        total += n;
        while (n) {
            const std::size_t take = std::min(n, sizeof block - fill);
            std::memcpy(block + fill, p, take);
            fill += take;
            p += take;
            n -= take;
            if (fill == sizeof block) { compress(block); fill = 0; }
        }
    }

    void finish(std::uint8_t out[20]) {
        const std::uint64_t bits = total * 8;
        const std::uint8_t one = 0x80;
        update(&one, 1);
        const std::uint8_t zero = 0;
        while (fill != 56) update(&zero, 1);
        std::uint8_t len[8];
        for (int i = 0; i < 8; ++i) len[i] = std::uint8_t(bits >> (56 - i * 8));
        // Bypass update() so the length does not extend `total`.
        std::memcpy(block + fill, len, 8);
        compress(block);
        for (int i = 0; i < 5; ++i) {
            out[i * 4]     = std::uint8_t(h[i] >> 24);
            out[i * 4 + 1] = std::uint8_t(h[i] >> 16);
            out[i * 4 + 2] = std::uint8_t(h[i] >> 8);
            out[i * 4 + 3] = std::uint8_t(h[i]);
        }
    }
};

bool hexNibble(char c, int& out) {
    if (c >= '0' && c <= '9') { out = c - '0'; return true; }
    if (c >= 'a' && c <= 'f') { out = c - 'a' + 10; return true; }
    if (c >= 'A' && c <= 'F') { out = c - 'A' + 10; return true; }
    return false;
}

// Parse a UUID string into its 16 network-order bytes.
bool uuidBytes(const std::string& s, std::uint8_t out[16]) {
    int n = 0;
    int hi = -1;
    for (char c : s) {
        if (c == '-') continue;
        int v;
        if (!hexNibble(c, v)) return false;
        if (hi < 0) { hi = v; continue; }
        if (n >= 16) return false;
        out[n++] = std::uint8_t(hi * 16 + v);
        hi = -1;
    }
    return n == 16 && hi < 0;
}

std::string format(const std::uint8_t b[16]) {
    char buf[37];
    std::snprintf(buf, sizeof buf,
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
                  "%02x%02x%02x%02x%02x%02x",
                  b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                  b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return buf;
}

}  // namespace

// A v4 UUID generated once and hard-coded, which is exactly how RFC 4122 says
// to mint an application namespace.
const char* const kPcapReplayNamespace = "6f2b1c84-7d3e-4a19-9c05-2b8e4f61d7a3";

std::string uuidV5(const std::string& ns, const std::string& name) {
    std::uint8_t nsb[16] = {};
    if (!uuidBytes(ns, nsb)) return {};

    Sha1 sha;
    sha.update(nsb, 16);
    sha.update(reinterpret_cast<const std::uint8_t*>(name.data()), name.size());
    std::uint8_t digest[20];
    sha.finish(digest);

    std::uint8_t out[16];
    std::memcpy(out, digest, 16);
    out[6] = std::uint8_t((out[6] & 0x0F) | 0x50);   // version 5
    out[8] = std::uint8_t((out[8] & 0x3F) | 0x80);   // RFC 4122 variant
    return format(out);
}

std::string resourceId(const std::string& seed, const std::string& role) {
    return uuidV5(kPcapReplayNamespace, seed + "/" + role);
}

std::string taiVersion() {
    using namespace std::chrono;
    const auto now = system_clock::now().time_since_epoch();
    const auto secs = duration_cast<seconds>(now);
    const auto nanos = duration_cast<nanoseconds>(now - secs);
    constexpr std::int64_t kTaiMinusUtc = 37;
    return std::to_string(secs.count() + kTaiMinusUtc) + ":" +
           std::to_string(nanos.count());
}

std::string uuidV4() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uint8_t b[16];
    for (int i = 0; i < 16; i += 8) {
        const std::uint64_t v = rng();
        std::memcpy(b + i, &v, 8);
    }
    b[6] = std::uint8_t((b[6] & 0x0F) | 0x40);
    b[8] = std::uint8_t((b[8] & 0x3F) | 0x80);
    return format(b);
}

}  // namespace pcapreplay::nmos
