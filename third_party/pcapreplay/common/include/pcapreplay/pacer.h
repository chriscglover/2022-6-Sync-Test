// Packet pacing.
//
// This is the hard part of the sender. 1080i25 is 134,925 packets/sec -- one
// every 7.41 us -- and 1080p50 is 269,850/sec, one every 3.71 us. No general
// purpose OS will sleep for that long. Windows Sleep(1) is 1-15 ms depending on
// the current timer resolution and a waitable timer bottoms out around 0.5 ms;
// at 1080i25 a 1 ms granularity is 135 packets of jitter. Linux does far better
// -- clock_nanosleep lands within tens of microseconds -- but tens of
// microseconds is still several packets.
//
// A naive sender bursts a whole frame at line rate and then idles. That looks
// perfect on a throughput graph and destroys any receiver with a realistically
// sized buffer -- exactly the "fine on the bench, fails under load" result that
// makes a reference rig worthless.
//
// So: absolute scheduling against a monotonic clock -- QueryPerformanceCounter
// on Windows, CLOCK_MONOTONIC on Linux -- never incremental, so error cannot
// accumulate. Coarse waits sleep, the final few microseconds spin. The
// threshold between the two differs per platform because the sleep accuracy
// does; the structure does not. One pacer drives both ST 2022-7 paths, so the A
// and B copies leave in the same slot.
//
// See docs/04-pacing-and-network.md.
#pragma once

#include <atomic>
#include <cstdint>

namespace pcapreplay {

struct PacerStats {
    std::int64_t packetsIssued = 0;
    std::int64_t slots         = 0;   // times acquire() returned
    std::int64_t sleeps        = 0;   // coarse waits taken
    std::int64_t spins         = 0;   // fine waits taken
    std::int64_t maxBacklog    = 0;   // worst credit owed in one acquire
    double       elapsedSeconds = 0.0;
    double       achievedPps   = 0.0;
    double       targetPps     = 0.0;

    // Worst observed lateness of a slot against its ideal time, microseconds.
    double       maxLatenessUs = 0.0;

    // Times the schedule was re-anchored because transmit debt exceeded what
    // could sensibly be made up. Non-zero means the transmitter stalled.
    std::int64_t resyncs = 0;
};

class SpinPacer {
public:
    SpinPacer();
    ~SpinPacer();

    SpinPacer(const SpinPacer&) = delete;
    SpinPacer& operator=(const SpinPacer&) = delete;

    // Begin (or restart) the schedule. Raises the process timer resolution.
    void start(double packetsPerSecond);
    void stop();

    bool running() const { return running_; }

    // Block until at least one packet is due, then return how many are owed,
    // capped at maxBurst. The caller must transmit exactly that many.
    //
    // Bursting a handful rather than placing every packet individually is
    // deliberate: at 1080p50 a sendto() costs a meaningful fraction of the
    // 3.71 us budget, so small bursts against an absolute schedule give better
    // real-world spacing than trying to pace one packet at a time.
    int acquire(int maxBurst = 8);

    // Abandon the current schedule position -- e.g. on a format change.
    void resync();

    PacerStats stats() const;

    // Pin the calling thread to time-critical priority. Call from the transmit
    // thread. Returns false if the priority could not be raised -- on Linux that
    // is the ordinary unprivileged case, since SCHED_FIFO needs CAP_SYS_NICE.
    // Replay still works; it is simply more exposed to what else the machine is
    // doing, so the caller is expected to say so rather than ignore it.
    static bool elevateCurrentThread();

    // Nominal packets/sec for a format: bytes per frame / 1376, times rate.
    static double packetsPerSecondFor(std::int64_t bytesPerFrame,
                                      double frameRate,
                                      std::size_t payloadBytes);

private:
    double        freq_        = 0.0;   // monotonic clock ticks per second
    std::int64_t  t0_          = 0;
    double        pps_         = 0.0;
    std::int64_t  issued_      = 0;
    bool          running_     = false;
    bool          timerRaised_ = false;

    std::atomic<std::int64_t> slots_{0};
    std::atomic<std::int64_t> sleeps_{0};
    std::atomic<std::int64_t> spins_{0};
    std::atomic<std::int64_t> maxBacklog_{0};
    std::atomic<double>       maxLatenessUs_{0.0};
    std::atomic<std::int64_t> resyncs_{0};
};

}  // namespace pcapreplay
