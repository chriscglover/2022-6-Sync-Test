// Composes the test signal and transmits it as ST 2022-6, optionally -7.
//
// Two threads. The producer pulls a picture, composes it and packs it into a
// small ring ahead of time. The transmitter paces datagrams against a monotonic
// clock, exactly as PCAP Replay does, and takes the next frame from the ring at
// each frame boundary. If the producer ever falls behind, the transmitter repeats
// the previous frame rather than leave a gap on the wire, and counts it.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "composer.h"

namespace testsignal {

struct SenderPath {
    std::string   group = "239.1.1.1";
    std::uint16_t port = 40000;
    std::string   interfaceIp;
};

struct SenderConfig {
    ComposerSettings composer;
    // When true the timecode on frame 0 is the local time of day at which frame
    // 0 is scheduled to leave; otherwise composer.timecodeStartFrames is used.
    bool          timecodeFromTimeOfDay = true;
    bool          enablePathB = false;
    SenderPath    pathA, pathB;
    int           ttl = 8;
    bool          loopback = true;
    int           ref = 3;               // HBRMT R field, as PCAP Replay sends
    std::uint32_t ssrc = 0x7E575160u;
    double        maxSeconds = 0.0;      // 0 = until stopped
};

struct SenderStatus {
    bool          running = false;
    bool          completed = false;
    std::string   error;
    std::string   warning;
    std::string   formatText;
    std::string   destinationA, destinationB;

    std::uint64_t framesSent = 0;
    std::uint64_t repeatedFrames = 0;
    double        elapsedSeconds = 0.0;
    std::uint64_t datagrams = 0;
    double        targetPps = 0.0;
    double        achievedPps = 0.0;
    double        wireMbps = 0.0;
    double        maxLatenessUs = 0.0;
    std::int64_t  pacerResyncs = 0;
    std::string   timecode;              // on the frame most recently started
    std::uint64_t audioOverflowPackets = 0;
    std::uint64_t runTag = 0;
};

class Sender {
public:
    Sender() = default;
    ~Sender();
    Sender(const Sender&) = delete;
    Sender& operator=(const Sender&) = delete;

    bool start(const SenderConfig& cfg);
    void stop();
    bool running() const { return running_.load(std::memory_order_relaxed); }

    SenderStatus status() const;
    SenderConfig activeConfig() const;

private:
    void run(SenderConfig cfg);

    std::thread       thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};

    mutable std::mutex mutex_;
    SenderStatus       status_;
    SenderConfig       config_;
};

}  // namespace testsignal
