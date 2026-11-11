// Teletext subtitles as a subtitle inserter transmits them: WST packets
// (ETS 300 706) carried in OP-47 subtitling distribution packets on HD
// (SMPTE RDD 8, DID 0x43 / SDID 0x02, luma VANC) and as the teletext waveform
// itself on the vertical blanking lines of 625-line SD (ITU-R BT.653 System B).
//
// The pager models what goes on air, not what a subtitler sends. A page cycle
// is the page header, the display rows, and a header for a page that does not
// exist, which tells a decoder the page is complete. An erase is a header with
// C4 set and no rows. While a caption or an erase is current it is sent again
// every repeat interval (C4 clear for a caption, so it rewrites in place), so
// a decoder that joins late still shows it. Broadcast inserters do the same:
// the order, the control bits and the repeats here follow real ST 2022-6
// captures of page 888.
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <span>
#include <vector>

namespace testsignal {

// One teletext packet as transmitted after the framing code: the two Hamming
// 8/4 magazine and row address bytes and 40 data bytes. Bytes are in the
// order they are sent, each least-significant bit first.
using TeletextPacket = std::array<std::uint8_t, 42>;

// Hamming 8/4 for a 4-bit value (ETS 300 706 §8.2), and its decode, which
// corrects a single-bit error. Returns -1 for an uncorrectable byte.
std::uint8_t hamming84(int nibble);
int          hamming84Decode(std::uint8_t byte);

// A 7-bit character with its odd-parity bit in b7.
std::uint8_t oddParity(std::uint8_t c);

// Page control bits carried in a page header (X/0), ETS 300 706 §9.3.1.
struct PageControl {
    bool erase = false;           // C4
    bool subtitle = true;         // C6
    bool suppressHeader = true;   // C7
    bool interrupted = true;      // C9
};

// `page` is three hex digits, magazine first: 0x888 is page 888, 0x8FF is the
// time-filling page of magazine 8. Magazine 8 is transmitted as 0.
TeletextPacket headerPacket(int page, const PageControl& control);
TeletextPacket rowPacket(int magazine, int row, std::span<const std::uint8_t, 40> cells);

// One OP-47 subtitling distribution packet holding up to five teletext
// packets: the identifier 0x51 0x15, length, format code 0x02 (WST),
// five line descriptors, the 45-byte data blocks (run-in 0x55 0x55, framing
// code 0x27, packet), and the footer 0x74 with a 16-bit sequence counter and
// a checksum that makes all the bytes sum to zero. Returns the user data
// words; the caller wraps them in an ANC packet.
std::vector<std::uint8_t> op47Sdp(std::span<const TeletextPacket> packets, std::uint16_t sequence);

// One 625-line blanking line (720 luma samples, 10-bit) carrying `packet` as
// the 6.9375 Mbit/s teletext waveform: black at 64, a one at 66% of white,
// raised-cosine edges, first bit 6 samples into the active line.
std::array<std::uint16_t, 720> wstLine(const TeletextPacket& packet);

struct TeletextStats {
    std::uint64_t reveals = 0;
    std::uint64_t erases = 0;
    std::uint64_t repeats = 0;
    std::uint64_t packetsSent = 0;
    std::uint64_t coalesced = 0;   // cycles merged into one not yet on air
};

// The inserter's page state and transmit queue. NEWFOR events arrive on one
// thread; takeFrame() is called once per video frame on another.
class TeletextPager {
public:
    // `repeatFrames` is the frames between repeats of the current page, 0 for
    // none.
    explicit TeletextPager(int repeatFrames = 50, int page = 0x888);

    // NEWFOR events.
    void connect(int page);                          // page as 0x888
    void build(bool clearFirst, int row, std::span<const std::uint8_t, 40> cells);
    void reveal();                                   // put the built page on air
    void clear();                                    // erase the display
    void disconnect();                               // erase what is on air

    // Up to `max` packets for the next frame. Advances the repeat clock by
    // one frame.
    std::vector<TeletextPacket> takeFrame(int max);

    int page() const;
    TeletextStats stats() const;

private:
    using Rows = std::map<int, std::array<std::uint8_t, 40>>;
    struct Cycle {
        int  page;
        bool erase;
        Rows rows;
        std::vector<TeletextPacket> packets;   // filled when it starts to go out
        std::size_t next = 0;
    };

    void queue(int page, bool erase, const Rows& rows, bool isRepeat);
    void eraseOnAir(int page);

    mutable std::mutex mutex_;
    int  repeatFrames_;
    int  page_;
    Rows built_;
    bool buildErase_ = false;   // a BUILD asked for clear-before-display
    // What a decoder shows once everything queued is on air.
    int  airPage_ = -1;
    Rows onAir_;
    bool airIsErase_ = false;
    bool haveAir_ = false;
    int  sinceSent_ = 0;        // frames since the page last went out
    std::deque<Cycle> queue_;
    TeletextStats stats_;
};

}  // namespace testsignal
