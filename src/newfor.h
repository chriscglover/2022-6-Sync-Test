// A NEWFOR subtitle link, inserter end: a subtitler connects over TCP and
// sends CONNECT, BUILD, REVEAL and CLEAR; each one is handed to the teletext
// pager, which puts it on air.
//
//   CONNECT  0x0E 0x00 [mag_H] [tens_H] [units_H]     page; 999 = DISCONNECT
//   BUILD    0x0F [ctl_H] { [row_hi_H] [row_lo_H] [40 bytes, odd parity] } x N
//            ctl bit 3 = clear before display, bits 2..0 = N
//   REVEAL   0x10
//   CLEAR    0x18
//
// _H is a Hamming 8/4 byte. Command bytes may arrive with odd parity set
// (0x8F, 0x98), so they are matched on their low seven bits. The inserter
// sends nothing back.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "teletext.h"

namespace testsignal {

// Turns the byte stream of one NEWFOR session into pager calls. Bytes may
// arrive in any split; a command is acted on once it is complete.
class NewforParser {
public:
    using Log = std::function<void(const std::string&)>;

    NewforParser(TeletextPager& pager, Log log = {});

    void feed(std::span<const std::uint8_t> bytes);
    bool disconnected() const { return disconnected_; }
    std::uint64_t commands() const { return commands_; }
    std::uint64_t unknownBytes() const { return unknown_; }

private:
    // Bytes the command at the head of the buffer needs, or 0 if more must
    // arrive before that is known.
    std::size_t commandLength() const;
    void        execute(std::size_t length);

    TeletextPager&            pager_;
    Log                       log_;
    std::vector<std::uint8_t> buf_;
    bool                      disconnected_ = false;
    std::uint64_t             commands_ = 0;
    std::uint64_t             unknown_ = 0;
};

struct NewforStatus {
    bool          listening = false;
    std::string   error;
    std::string   peer;            // the connected subtitler, empty when none
    std::uint64_t sessions = 0;
    std::uint64_t commands = 0;
};

// Listens for one subtitler at a time; a second connection waits until the
// first closes. Closing a session takes its caption off air.
class NewforServer {
public:
    explicit NewforServer(TeletextPager& pager) : pager_(pager) {}
    ~NewforServer();
    NewforServer(const NewforServer&) = delete;
    NewforServer& operator=(const NewforServer&) = delete;

    // `bindAddress` empty = all interfaces. Reports to stdout when `verbose`.
    bool start(const std::string& bindAddress, int port, bool verbose, std::string& error);
    void stop();
    NewforStatus status() const;

private:
    void run();

    TeletextPager&    pager_;
    int               listenFd_ = -1;
    bool              verbose_ = false;
    std::thread       thread_;
    std::atomic<bool> stopping_{false};
    mutable std::mutex mutex_;
    NewforStatus      status_;
};

}  // namespace testsignal
