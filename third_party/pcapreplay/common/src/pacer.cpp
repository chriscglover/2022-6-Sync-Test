#include "pcapreplay/pacer.h"

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#else
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <cerrno>
#include <immintrin.h>
#endif

#include <algorithm>
#include <cmath>

namespace pcapreplay {
namespace {

#ifdef _WIN32

inline std::int64_t qpc() {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

inline double qpf() {
    LARGE_INTEGER v;
    QueryPerformanceFrequency(&v);
    return double(v.QuadPart);
}

// Below this, sleeping overshoots and we spin instead. 1.5 ms leaves room for
// a 1 ms timer granularity plus scheduling slop.
constexpr double kSpinThresholdSeconds = 0.0015;

// Coarse wait. Windows cannot sleep for less than the current timer resolution,
// so the caller only ever asks for the part of the wait that is well clear of it.
inline void coarseWait(double seconds) {
    const DWORD ms = DWORD(seconds * 1000.0);
    Sleep(ms ? ms : 1);
}

inline void spinHint() { YieldProcessor(); }

#else   // POSIX

// CLOCK_MONOTONIC is the direct equivalent of QueryPerformanceCounter: it is
// nanosecond-resolution, it does not step when the wall clock is adjusted, and
// on any modern x86 kernel it is a vDSO read of the TSC with no syscall at all.
// That last point matters here -- this is read tens of thousands of times a
// second on the transmit thread's hot path.
inline std::int64_t qpc() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return std::int64_t(ts.tv_sec) * 1000000000ll + ts.tv_nsec;
}

inline double qpf() { return 1e9; }

// Linux sleeps far more accurately than Windows does: clock_nanosleep against
// an absolute CLOCK_MONOTONIC deadline lands within tens of microseconds on an
// ordinary kernel, against Windows' 1-15 ms. So the spin threshold can be an
// order of magnitude tighter, which is most of what makes the spin loop cheap
// here -- fewer packets are placed by burning CPU and more by sleeping to them.
constexpr double kSpinThresholdSeconds = 0.00015;

inline void coarseWait(double seconds) {
    if (seconds <= 0.0) return;
    // Absolute deadline rather than a relative sleep: a relative one restarts
    // its interval if a signal interrupts it, which would quietly stretch every
    // wait the process ever takes a signal during.
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    const std::int64_t deadline =
        std::int64_t(now.tv_sec) * 1000000000ll + now.tv_nsec +
        std::int64_t(seconds * 1e9);
    timespec until{};
    until.tv_sec  = time_t(deadline / 1000000000ll);
    until.tv_nsec = long(deadline % 1000000000ll);
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &until, nullptr) == EINTR) {}
}

inline void spinHint() { _mm_pause(); }

#endif

// How much transmit debt the pacer will try to make up before re-anchoring.
//
// This exists to stop a stalled transmitter accruing unbounded debt (7.6
// million packets was observed once), after which the pacer returns maximum
// burst forever and stops pacing at all.
//
// 50 ms proved far too tight: ordinary scheduling jitter pushed the debt past
// it several times a second, and each clamp threw away schedule the thread had
// ample headroom to recover -- costing ~3% of the target rate and, with it,
// about a frame a second. 500 ms is still firmly bounded but lets normal
// hiccups be made up. Catch-up is inherently rate-limited because acquire()
// never returns more than one burst at a time.
constexpr double kMaxCatchUpSeconds = 0.500;

}  // namespace

SpinPacer::SpinPacer() : freq_(qpf()) {}

SpinPacer::~SpinPacer() { stop(); }

double SpinPacer::packetsPerSecondFor(std::int64_t bytesPerFrame,
                                      double frameRate,
                                      std::size_t payloadBytes) {
    if (payloadBytes == 0 || frameRate <= 0.0) return 0.0;
    const double perFrame =
        std::ceil(double(bytesPerFrame) / double(payloadBytes));
    return perFrame * frameRate;
}

void SpinPacer::start(double packetsPerSecond) {
    stop();
    pps_ = packetsPerSecond > 0.0 ? packetsPerSecond : 0.0;
    if (pps_ <= 0.0) return;

#ifdef _WIN32
    // Tighten the OS timer so the coarse part of each wait is not 15 ms.
    timerRaised_ = timeBeginPeriod(1) == TIMERR_NOERROR;
#else
    // Nothing to raise: clock_nanosleep already resolves far finer than the
    // spin threshold, and the process-wide timer slack a Linux thread inherits
    // (50 us by default) is below it too.
    timerRaised_ = false;
#endif

    t0_ = qpc();
    issued_ = 0;
    slots_.store(0);
    sleeps_.store(0);
    spins_.store(0);
    maxBacklog_.store(0);
    maxLatenessUs_.store(0.0);
    resyncs_.store(0);
    running_ = true;
}

void SpinPacer::stop() {
#ifdef _WIN32
    if (timerRaised_) {
        timeEndPeriod(1);
        timerRaised_ = false;
    }
#endif
    running_ = false;
}

void SpinPacer::resync() {
    t0_ = qpc();
    issued_ = 0;
}

int SpinPacer::acquire(int maxBurst) {
    if (!running_ || pps_ <= 0.0) return 0;
    if (maxBurst < 1) maxBurst = 1;

    for (;;) {
        const std::int64_t now = qpc();
        const double elapsed = double(now - t0_) / freq_;
        const std::int64_t due = std::int64_t(elapsed * pps_);
        std::int64_t credit = due - issued_;

        // Clamp the debt. If the transmitter stalls, `due` runs away from
        // `issued_` and the pacer would spend the rest of the session trying to
        // make up millions of packets -- returning maxBurst every time and
        // never actually pacing again. You cannot send a packet retroactively,
        // so past the catch-up limit we forgive the debt and resume pacing.
        // Observed backlog after the earlier stall: 7.6 million packets.
        const std::int64_t catchUpLimit = std::int64_t(pps_ * kMaxCatchUpSeconds) + 1;
        if (credit > catchUpLimit) {
            issued_ = due - catchUpLimit;
            credit = catchUpLimit;
            resyncs_.fetch_add(1, std::memory_order_relaxed);
        }

        if (credit >= 1) {
            // How late is the oldest packet in this slot against its ideal time?
            const double idealT = double(issued_) / pps_;
            const double lateUs = (elapsed - idealT) * 1e6;
            double prev = maxLatenessUs_.load(std::memory_order_relaxed);
            while (lateUs > prev &&
                   !maxLatenessUs_.compare_exchange_weak(prev, lateUs,
                                                         std::memory_order_relaxed)) {}

            std::int64_t prevBacklog = maxBacklog_.load(std::memory_order_relaxed);
            while (credit > prevBacklog &&
                   !maxBacklog_.compare_exchange_weak(prevBacklog, credit,
                                                      std::memory_order_relaxed)) {}

            if (credit > maxBurst) credit = maxBurst;
            issued_ += credit;
            slots_.fetch_add(1, std::memory_order_relaxed);
            return int(credit);
        }

        // Wait until the next packet is due. Absolute target, so a long sleep
        // that overshoots simply yields more credit next time round rather than
        // pushing the whole schedule late.
        const double nextIdeal = double(issued_ + 1) / pps_;
        const double waitS = nextIdeal - elapsed;

        if (waitS > kSpinThresholdSeconds) {
            sleeps_.fetch_add(1, std::memory_order_relaxed);
            coarseWait(waitS - kSpinThresholdSeconds);
        } else {
            spins_.fetch_add(1, std::memory_order_relaxed);
            // Spin, but let a hyperthread sibling make progress.
            spinHint();
        }
    }
}

bool SpinPacer::elevateCurrentThread() {
#ifdef _WIN32
    return SetThreadPriority(GetCurrentThread(),
                             THREAD_PRIORITY_TIME_CRITICAL) != 0;
#else
    // SCHED_FIFO is the equivalent of THREAD_PRIORITY_TIME_CRITICAL and it is
    // what keeps a 7.4 us packet interval from being interrupted by an ordinary
    // CFS timeslice. It needs CAP_SYS_NICE, which this tool deliberately does
    // not demand: unprivileged replay works, it is simply less immune to what
    // else the machine is doing.
    //
    // Grant it without running as root with either of:
    //   sudo setcap cap_sys_nice=eip ./replay
    //   sudo chrt -f 10 ./replay ...
    //
    // Failure is reported to the caller rather than swallowed, so the CLI can
    // say the pacing is best-effort instead of leaving it to be discovered as
    // jitter.
    sched_param sp{};
    sp.sched_priority = 10;     // low within SCHED_FIFO: real time, not greedy
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0) return true;

    // Fall back to the best nice level the process is allowed. Worth having:
    // most of the benefit is in not being preempted by background work, and
    // this part needs no privilege at all.
    sp.sched_priority = 0;
    pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);
    return false;
#endif
}

PacerStats SpinPacer::stats() const {
    PacerStats s;
    s.packetsIssued = issued_;
    s.slots      = slots_.load(std::memory_order_relaxed);
    s.sleeps     = sleeps_.load(std::memory_order_relaxed);
    s.spins      = spins_.load(std::memory_order_relaxed);
    s.maxBacklog = maxBacklog_.load(std::memory_order_relaxed);
    s.maxLatenessUs = maxLatenessUs_.load(std::memory_order_relaxed);
    s.resyncs    = resyncs_.load(std::memory_order_relaxed);
    s.targetPps  = pps_;
    if (running_ && freq_ > 0.0) {
        s.elapsedSeconds = double(qpc() - t0_) / freq_;
        if (s.elapsedSeconds > 0.0)
            s.achievedPps = double(issued_) / s.elapsedSeconds;
    }
    return s;
}

}  // namespace pcapreplay
