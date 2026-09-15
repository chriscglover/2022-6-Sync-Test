// ST 2022-6/-7 reception for the delay probe.
//
// Each leg is read on its own thread. Datagrams are merged by RTP sequence
// number -- the first copy of each wins and keeps its arrival time -- and frames
// are cut on the RTP marker bit. A completed packed frame is handed to a worker
// thread that recovers its luma and embedded audio, so unpacking never delays
// the receive threads and therefore never shifts an arrival stamp.
//
// Frames are read in wire order, as ST 2022-6 equipment carries them: cut at
// line 1's EAV, each line [EAV][LN][CRC][HANC][SAV][active].
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "probe/probe_types.h"

namespace testsignal {

struct St2022Leg {
    std::string   group;
    std::uint16_t port = 40000;
    std::string   interfaceIp;
};

struct St2022SourceConfig {
    St2022Leg a;
    bool      haveB = false;
    St2022Leg b;
};

struct St2022Stats {
    std::uint64_t datagramsA = 0, datagramsB = 0;
    std::uint64_t duplicates = 0;      // second copies and late arrivals
    std::uint64_t missing = 0;         // sequence numbers neither leg delivered
    std::uint64_t foreign = 0;         // datagrams from another stream (SSRC)
    std::uint64_t framesOk = 0;
    std::uint64_t framesBad = 0;       // incomplete, unknown format or not EAV-first
    std::uint64_t framesDroppedBusy = 0;
    std::string   format;
    std::string   lastProblem;
};

// A frame as it came off the wire, before luma and audio recovery.
struct PackedFrame {
    std::vector<std::uint8_t> bytes;
    std::int64_t  arrivalNs = 0;
    std::uint8_t  frameCode = 0, frateCode = 0, sampleCode = 0;
    bool          complete = true;
};

// Sequence merge and frame cutting. Not thread-safe; the receiver serialises it.
class St2022Merger {
public:
    using PackedSink = std::function<void(PackedFrame&&)>;

    explicit St2022Merger(PackedSink sink, int window = 8192);

    // One datagram from either leg. Returns false if it was not taken.
    bool push(const std::uint8_t* data, std::size_t len, std::int64_t arrivalNs);

    std::uint64_t duplicates() const { return duplicates_; }
    std::uint64_t missing() const { return missing_; }
    std::uint64_t foreign() const { return foreign_; }

private:
    struct Slot {
        bool          present = false;
        std::uint64_t ext = 0;
        bool          marker = false;
        std::int64_t  arrivalNs = 0;
        std::uint8_t  frameCode = 0, frateCode = 0, sampleCode = 0;
        std::uint8_t  payload[1376];
    };

    void advance();                // emit or skip the slot at next_
    void emit(const Slot& slot);

    PackedSink        sink_;
    std::vector<Slot> slots_;
    std::uint64_t     window_;
    bool              haveSeq_ = false;
    std::uint64_t     highest_ = 0;
    std::uint64_t     next_ = 0;
    std::uint64_t     duplicates_ = 0, missing_ = 0, foreign_ = 0;
    bool              haveSsrc_ = false;
    std::uint32_t     ssrc_ = 0;
    bool              synced_ = false;   // a marker has been seen; frames start clean
    PackedFrame       frame_;
    bool              frameStarted_ = false;
};

// Recover the active-picture luma of a wire-order frame.
bool extractLuma(const PackedFrame& packed, LumaFrame& out, std::string& problem);

// Recover the first channel of embedded audio group 1 (ST 299 on HD, ST 272 on
// SD), timed from the frame's arrival. Returns false when the frame carries none.
bool extractAudio(const PackedFrame& packed, AudioBlock& out);

class St2022Receiver {
public:
    St2022Receiver() = default;
    ~St2022Receiver();
    St2022Receiver(const St2022Receiver&) = delete;
    St2022Receiver& operator=(const St2022Receiver&) = delete;

    bool start(const St2022SourceConfig& cfg, FrameSink frames, AudioSink audio, std::string& error);
    void stop();
    St2022Stats stats() const;

private:
    void receiveLoop(int leg, const St2022Leg& cfg);
    void workerLoop();

    FrameSink                frameSink_;
    AudioSink                audioSink_;
    std::atomic<bool>        stopping_{false};
    std::vector<std::thread> threads_;

    mutable std::mutex       mergeMutex_;
    std::unique_ptr<St2022Merger> merger_;

    mutable std::mutex       queueMutex_;
    std::condition_variable  queueCv_;
    std::deque<PackedFrame>  queue_;

    mutable std::mutex       statsMutex_;
    St2022Stats              stats_;
    std::atomic<std::uint64_t> datagrams_[2] = {0, 0};
};

}  // namespace testsignal
