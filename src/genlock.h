// ST 2059-1 frame alignment for the ST 2022-6 sender.
//
// A PTP-locked analyser judges an ST 2022-6 stream against its own reference:
// it expects frame k of a rate num/den to start k * den/num seconds after the
// PTP epoch (1970-01-01 00:00:00 TAI), and the RTP timestamp to be that
// instant on a 27 MHz clock, modulo 2^32. A sender that starts its timestamps
// at zero, or advances them per frame sent rather than per frame period, has no
// fixed relation to that reference; an analyser then reports a wrong RTP clock
// rate or a drifting offset even though the stream decodes.
//
// The grid here places every frame by its index since the epoch. The sender
// takes the time from the system clock, so lock that clock to PTP (phc2sys,
// or chrony with a PHC refclock) for the timestamps to agree with a PTP
// reference. All arithmetic is exact integers: a 59.94 Hz frame period is not
// a whole number of nanoseconds, and rounding it once would creep.
#pragma once

#include <cstdint>
#include <ctime>

namespace testsignal {

class St2059Grid {
public:
    static constexpr std::uint64_t kNs = 1'000'000'000;
    static constexpr std::uint64_t kRtpHz = 27'000'000;
    // TAI - UTC since 2017-01-01, used when the kernel holds no TAI offset
    // (phc2sys sets it with -a / -O, chrony only with leapsectz).
    static constexpr std::int64_t kDefaultTaiOffsetS = 37;

    // `clockNum / clockDen` Hz is the HBRMT video timestamp clock (the CF
    // field): 148.5 MHz, or 148.5 / 1.001 MHz. Zero when CF is 0.
    St2059Grid(std::uint64_t frameRateNum, std::uint64_t frameRateDen,
               std::uint64_t clockNum, std::uint64_t clockDen, std::int64_t taiOffsetS)
        : num_(frameRateNum), den_(frameRateDen), clockNum_(clockNum),
          clockDen_(clockDen ? clockDen : 1), taiOffsetS_(taiOffsetS) {}

    std::int64_t taiOffsetS() const { return taiOffsetS_; }

    // Start of frame `index`, nanoseconds since the PTP epoch, rounded to nearest.
    std::uint64_t frameStartNs(std::uint64_t index) const {
        const unsigned __int128 n = (unsigned __int128)index * den_ * kNs;
        return std::uint64_t((n + num_ / 2) / num_);
    }

    // The first frame starting at or after `taiNs`.
    std::uint64_t indexAtOrAfter(std::uint64_t taiNs) const {
        const unsigned __int128 scaled = (unsigned __int128)taiNs * num_;
        const unsigned __int128 period = (unsigned __int128)den_ * kNs;
        return std::uint64_t((scaled + period - 1) / period);
    }

    // RTP timestamp of frame `index`: 27 MHz since the epoch, modulo 2^32.
    std::uint32_t rtpTimestamp(std::uint64_t index) const {
        return std::uint32_t((unsigned __int128)index * den_ * kRtpHz / num_);
    }

    // HBRMT video timestamp of frame `index` on the CF clock, modulo 2^32.
    std::uint32_t videoTimestamp(std::uint64_t index) const {
        if (clockNum_ == 0) return 0;
        return std::uint32_t((unsigned __int128)index * den_ * clockNum_ / (num_ * clockDen_));
    }

    // UTC nanoseconds (CLOCK_REALTIME) <-> TAI nanoseconds on the PTP timeline.
    std::uint64_t taiFromUtcNs(std::int64_t utcNs) const {
        const std::int64_t tai = utcNs + taiOffsetS_ * std::int64_t(kNs);
        return tai > 0 ? std::uint64_t(tai) : 0;
    }
    std::int64_t utcFromTaiNs(std::uint64_t taiNs) const {
        return std::int64_t(taiNs) - taiOffsetS_ * std::int64_t(kNs);
    }

    // TAI - UTC as the kernel holds it, in whole seconds; 0 when unset.
    static std::int64_t kernelTaiOffsetS() {
        timespec tai{}, utc{};
        clock_gettime(CLOCK_TAI, &tai);
        clock_gettime(CLOCK_REALTIME, &utc);
        const std::int64_t d = (std::int64_t(tai.tv_sec) - std::int64_t(utc.tv_sec)) * std::int64_t(kNs) +
                               (std::int64_t(tai.tv_nsec) - std::int64_t(utc.tv_nsec));
        return (d + std::int64_t(kNs) / 2) / std::int64_t(kNs);
    }

    static std::int64_t utcNowNs() {
        timespec ts{};
        clock_gettime(CLOCK_REALTIME, &ts);
        return std::int64_t(ts.tv_sec) * std::int64_t(kNs) + ts.tv_nsec;
    }

private:
    std::uint64_t num_, den_, clockNum_, clockDen_;
    std::int64_t  taiOffsetS_;
};

}  // namespace testsignal
