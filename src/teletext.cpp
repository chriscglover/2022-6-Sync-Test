#include "teletext.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace testsignal {

namespace {

// Hamming 8/4 codewords for 0..15, as transmitted (b0 first). Data bits ride
// b1, b3, b5 and b7.
constexpr std::uint8_t kHamming84[16] = {
    0x15, 0x02, 0x49, 0x5E, 0x64, 0x73, 0x38, 0x2F,
    0xD0, 0xC7, 0x8C, 0x9B, 0xA1, 0xB6, 0xFD, 0xEA,
};

constexpr double kPi = 3.14159265358979323846;

// The time-filling page of a magazine: a header for it ends the page cycle
// before it, and no decoder ever displays it.
int fillerPage(int page) { return (page & 0xF00) | 0xFF; }

std::uint8_t magazineOf(int page) { return std::uint8_t((page >> 8) & 0x07); }

void appendCycle(std::vector<TeletextPacket>& out, int page, bool erase,
                 const std::map<int, std::array<std::uint8_t, 40>>& rows) {
    PageControl pc;
    pc.erase = erase;
    out.push_back(headerPacket(page, pc));
    for (const auto& [row, cells] : rows)
        out.push_back(rowPacket(magazineOf(page), row, std::span<const std::uint8_t, 40>(cells)));
    out.push_back(headerPacket(fillerPage(page), PageControl{}));
}

}  // namespace

std::uint8_t hamming84(int nibble) { return kHamming84[nibble & 0x0F]; }

int hamming84Decode(std::uint8_t byte) {
    for (int n = 0; n < 16; ++n)
        if (kHamming84[n] == byte) return n;
    for (int n = 0; n < 16; ++n)
        if (std::popcount(unsigned(kHamming84[n] ^ byte)) == 1) return n;
    return -1;
}

std::uint8_t oddParity(std::uint8_t c) {
    c &= 0x7F;
    return std::uint8_t((std::popcount(unsigned(c)) & 1) ? c : (c | 0x80));
}

TeletextPacket headerPacket(int page, const PageControl& control) {
    TeletextPacket p{};
    const int mag = magazineOf(page);
    p[0] = hamming84(mag);              // packet 0: row bit 0 is clear
    p[1] = hamming84(0);
    p[2] = hamming84(page & 0x0F);      // page units
    p[3] = hamming84((page >> 4) & 0x0F);   // page tens
    p[4] = hamming84(0);                // S1
    p[5] = hamming84(control.erase ? 0x8 : 0);   // S2, C4
    p[6] = hamming84(0);                // S3
    p[7] = hamming84(control.subtitle ? 0x8 : 0);   // S4, C5, C6
    p[8] = hamming84((control.suppressHeader ? 0x1 : 0) | (control.interrupted ? 0x4 : 0));  // C7..C10
    p[9] = hamming84(0);                // C11 parallel transmission, C12..C14 English
    for (int i = 10; i < 42; ++i) p[std::size_t(i)] = oddParity(' ');
    return p;
}

TeletextPacket rowPacket(int magazine, int row, std::span<const std::uint8_t, 40> cells) {
    TeletextPacket p{};
    p[0] = hamming84((magazine & 0x07) | ((row & 1) << 3));
    p[1] = hamming84((row >> 1) & 0x0F);
    for (int i = 0; i < 40; ++i) p[std::size_t(2 + i)] = oddParity(cells[std::size_t(i)]);
    return p;
}

std::vector<std::uint8_t> op47Sdp(std::span<const TeletextPacket> packets, std::uint16_t sequence) {
    const std::size_t n = std::min<std::size_t>(packets.size(), 5);
    std::vector<std::uint8_t> b;
    b.reserve(9 + 45 * n + 4);
    b.push_back(0x51);
    b.push_back(0x15);
    b.push_back(std::uint8_t(9 + 45 * n + 4));   // every byte, identifier to checksum
    b.push_back(0x02);                           // WST teletext subtitles
    // The VBI lines these packets would occupy in 625-line video, ending on
    // line 22 of the second field as broadcast inserters place them.
    for (std::size_t i = 0; i < 5; ++i)
        b.push_back(i < n ? std::uint8_t(23 - int(n) + int(i)) : 0x00);
    for (std::size_t i = 0; i < n; ++i) {
        b.push_back(0x55);
        b.push_back(0x55);
        b.push_back(0x27);
        b.insert(b.end(), packets[i].begin(), packets[i].end());
    }
    b.push_back(0x74);
    b.push_back(std::uint8_t(sequence >> 8));
    b.push_back(std::uint8_t(sequence & 0xFF));
    unsigned sum = 0;
    for (std::uint8_t v : b) sum += v;
    b.push_back(std::uint8_t((256 - (sum & 0xFF)) & 0xFF));
    return b;
}

std::array<std::uint16_t, 720> wstLine(const TeletextPacket& packet) {
    // 13.5 MHz luma sampling against the 6.9375 Mbit/s teletext bit rate.
    constexpr double kSamplesPerBit = 13.5e6 / 6.9375e6;
    constexpr double kFirstBit = 6.0;
    constexpr double kBlack = 64.0;
    constexpr double kOne = 0.66 * (940.0 - 64.0);

    std::array<std::uint8_t, 45> bytes{};
    bytes[0] = 0x55;
    bytes[1] = 0x55;
    bytes[2] = 0x27;
    std::copy(packet.begin(), packet.end(), bytes.begin() + 3);

    std::array<double, 720> level{};
    for (int bit = 0; bit < 45 * 8; ++bit) {
        if (!((bytes[std::size_t(bit / 8)] >> (bit % 8)) & 1)) continue;
        // A raised-cosine pulse a bit period either side of the bit's centre:
        // adjacent ones sum to a flat top.
        const double centre = kFirstBit + bit * kSamplesPerBit;
        const int lo = std::max(0, int(std::ceil(centre - kSamplesPerBit)));
        const int hi = std::min(719, int(std::floor(centre + kSamplesPerBit)));
        for (int s = lo; s <= hi; ++s) {
            const double c = std::cos(kPi * (double(s) - centre) / (2.0 * kSamplesPerBit));
            level[std::size_t(s)] += c * c;
        }
    }
    std::array<std::uint16_t, 720> out{};
    for (int s = 0; s < 720; ++s)
        out[std::size_t(s)] = std::uint16_t(std::lround(kBlack + kOne * std::min(1.0, level[std::size_t(s)])));
    return out;
}

// ---- pager -----------------------------------------------------------------

TeletextPager::TeletextPager(int repeatFrames, int page)
    : repeatFrames_(std::max(0, repeatFrames)), page_(page) {}

int TeletextPager::page() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return page_;
}

TeletextStats TeletextPager::stats() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return stats_;
}

void TeletextPager::queue(int page, bool erase, const Rows& rows, bool isRepeat) {
    // A cycle that has not started to go out yet is merged with this one, so a
    // burst of NEWFOR commands within a frame or two becomes one transmission
    // with the same end result, not a queue of stale pages.
    Cycle* back = queue_.empty() ? nullptr : &queue_.back();
    if (back && back->packets.empty() && back->page == page) {
        if (erase) {
            back->erase = true;
            back->rows = rows;
        } else {
            for (const auto& [r, cells] : rows) back->rows[r] = cells;
        }
        ++stats_.coalesced;
    } else {
        queue_.push_back(Cycle{page, erase, rows, {}, 0});
    }

    if (!isRepeat) {
        if (airPage_ != page || erase) onAir_.clear();
        for (const auto& [r, cells] : rows) onAir_[r] = cells;
        airPage_ = page;
        airIsErase_ = onAir_.empty();
        haveAir_ = true;
    }
}

void TeletextPager::eraseOnAir(int page) {
    if (haveAir_ && airPage_ == page && !airIsErase_) {
        queue(page, true, {}, false);
        ++stats_.erases;
    }
}

void TeletextPager::connect(int page) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (page == page_) return;
    // A caption left on the old page would stay on screen there for good.
    eraseOnAir(page_);
    page_ = page;
    built_.clear();
    buildErase_ = false;
}

void TeletextPager::build(bool clearFirst, int row, std::span<const std::uint8_t, 40> cells) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (clearFirst) {
        built_.clear();
        buildErase_ = true;
    }
    if (row < 1 || row > 24) return;   // row 0 is the header; 25+ are not display rows
    std::array<std::uint8_t, 40> c{};
    for (int i = 0; i < 40; ++i) c[std::size_t(i)] = oddParity(cells[std::size_t(i)]);
    built_[row] = c;
}

void TeletextPager::reveal() {
    std::lock_guard<std::mutex> lk(mutex_);
    // An empty page has nothing to show: on air that is an erase.
    const bool erase = buildErase_ || built_.empty();
    queue(page_, erase, built_, false);
    buildErase_ = false;
    if (built_.empty()) ++stats_.erases;
    else ++stats_.reveals;
}

void TeletextPager::clear() {
    std::lock_guard<std::mutex> lk(mutex_);
    built_.clear();
    buildErase_ = false;
    queue(page_, true, {}, false);
    ++stats_.erases;
}

void TeletextPager::disconnect() {
    std::lock_guard<std::mutex> lk(mutex_);
    eraseOnAir(page_);
    built_.clear();
    buildErase_ = false;
}

std::vector<TeletextPacket> TeletextPager::takeFrame(int max) {
    std::lock_guard<std::mutex> lk(mutex_);
    ++sinceSent_;
    if (queue_.empty() && haveAir_ && repeatFrames_ > 0 && sinceSent_ >= repeatFrames_) {
        // A caption repeats with C4 clear, rewriting its rows in place; an
        // erase repeats as an erase.
        queue_.push_back(Cycle{airPage_, airIsErase_, airIsErase_ ? Rows{} : onAir_, {}, 0});
        ++stats_.repeats;
    }

    std::vector<TeletextPacket> out;
    while (int(out.size()) < max && !queue_.empty()) {
        Cycle& c = queue_.front();
        if (c.packets.empty()) appendCycle(c.packets, c.page, c.erase, c.rows);
        out.push_back(c.packets[c.next++]);
        if (c.next == c.packets.size()) {
            queue_.pop_front();
            sinceSent_ = 0;
        }
    }
    stats_.packetsSent += out.size();
    return out;
}

}  // namespace testsignal
