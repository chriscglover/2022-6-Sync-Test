#include "sender.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <vector>

#include "genlock.h"
#include "pcapreplay/hbrmt.h"
#include "pcapreplay/net_multicast.h"
#include "pcapreplay/pacer.h"
#include "picture_source.h"

namespace testsignal {

using namespace pcapreplay;
using Clock = std::chrono::system_clock;

namespace {

// Frames ahead the producer may run. The transmitter always holds one slot, so
// this leaves depth - 1 composed frames of slack.
constexpr int kRingDepth = 6;

// Time allowed for GStreamer to start and the ring to fill before frame 0 is
// due. Frame 0's marker time and time-of-day timecode are fixed from it.
constexpr std::chrono::milliseconds kPreroll{1500};

std::int64_t timeOfDayFrames(Clock::time_point t, TimecodeRate rate) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
    const std::time_t secs = std::time_t(ms / 1000);
    std::tm lt{};
    localtime_r(&secs, &lt);
    Timecode tc;
    tc.h = lt.tm_hour;
    tc.m = lt.tm_min;
    tc.s = lt.tm_sec;
    return framesFromTimecode(tc, rate) + (ms % 1000) * rate.fps / 1000;
}

}  // namespace

Sender::~Sender() { stop(); }

bool Sender::start(const SenderConfig& cfg) {
    stop();
    {
        std::lock_guard<std::mutex> lk(mutex_);
        status_ = SenderStatus{};
        config_ = cfg;
    }
    stopping_.store(false);
    running_.store(true);
    thread_ = std::thread([this, cfg] { run(cfg); });
    return true;
}

void Sender::stop() {
    stopping_.store(true);
    if (thread_.joinable()) thread_.join();
    running_.store(false);
}

SenderStatus Sender::status() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return status_;
}

SenderConfig Sender::activeConfig() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return config_;
}

void Sender::run(SenderConfig cfg) {
    auto fail = [&](const std::string& msg) {
        std::lock_guard<std::mutex> lk(mutex_);
        status_.running = false;
        status_.error = msg;
        running_.store(false, std::memory_order_relaxed);
    };

    const SdiFormatInfo fi = formatInfo(cfg.composer.format);
    if (fi.id == SdiFormat::Unknown) { fail("unsupported format"); return; }

    const std::size_t frameBytes = std::size_t(fi.bytesPerFrame());
    const std::uint64_t frameBits = std::uint64_t(frameBytes) * 8;
    const std::uint64_t dgPerFrame =
        (std::uint64_t(frameBytes) + kHbrmtPayloadBytes - 1) / kHbrmtPayloadBytes;
    const double pps = double(dgPerFrame) * fi.frameRate();
    const double frameSeconds = double(fi.frameRateDen) / double(fi.frameRateNum);

    // Frames sit on the ST 2059-1 grid: frame 0 is the first frame period that
    // starts after the preroll, by the system clock taken as PTP time, and
    // every RTP / video timestamp is its frame's place on that grid.
    const std::int64_t kernelTaiOffset = St2059Grid::kernelTaiOffsetS();
    const std::uint64_t cfClockNum = fi.cfCode == 2 ? 148'500'000ull : (fi.cfCode == 1 ? 148'500'000'000ull : 0);
    const std::uint64_t cfClockDen = fi.cfCode == 1 ? 1001 : 1;
    const St2059Grid grid(std::uint64_t(fi.frameRateNum), std::uint64_t(fi.frameRateDen), cfClockNum, cfClockDen,
                          kernelTaiOffset ? kernelTaiOffset : St2059Grid::kDefaultTaiOffsetS);
    auto utcPoint = [&](std::uint64_t gridIndex) {
        return Clock::time_point(std::chrono::duration_cast<Clock::duration>(
            std::chrono::nanoseconds(grid.utcFromTaiNs(grid.frameStartNs(gridIndex)))));
    };
    const std::uint64_t firstGridIndex = grid.indexAtOrAfter(grid.taiFromUtcNs(
        St2059Grid::utcNowNs() + std::chrono::duration_cast<std::chrono::nanoseconds>(kPreroll).count()));
    const Clock::time_point t0 = utcPoint(firstGridIndex);
    if (cfg.timecodeFromTimeOfDay)
        cfg.composer.timecodeStartFrames =
            timeOfDayFrames(t0, timecodeRate(fi.frameRateNum, fi.frameRateDen));
    const std::int64_t t0Ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(t0.time_since_epoch()).count();

    // ---- producer -----------------------------------------------------------
    std::vector<std::vector<std::uint8_t>> ring(kRingDepth, std::vector<std::uint8_t>(frameBytes));
    std::mutex ringMutex;
    std::condition_variable ringCv;
    std::uint64_t produced = 0, taken = 0;
    bool producerDone = false;
    std::string producerError;
    std::atomic<std::uint64_t> audioOverflow{0};

    std::thread producer([&] {
        std::string err;
        PictureSource source;
        if (!source.open(fi.activeWidth, fi.activeHeight, fi.frameRateNum, fi.frameRateDen,
                         cfg.composer.backgroundRgb, err)) {
            std::lock_guard<std::mutex> lk(ringMutex);
            producerError = err;
            producerDone = true;
            ringCv.notify_all();
            return;
        }
        FrameComposer composer(cfg.composer);
        std::vector<std::uint8_t> picture(std::size_t(fi.activeWidth) * 2 * std::size_t(fi.activeHeight));
        const UyvyImage image{picture.data(), fi.activeWidth, fi.activeHeight, fi.activeWidth * 2};

        for (std::uint64_t k = 0;; ++k) {
            {
                std::unique_lock<std::mutex> lk(ringMutex);
                // Never write the slot the transmitter is sending from.
                ringCv.wait(lk, [&] {
                    return stopping_.load() || produced + 1 < taken + kRingDepth;
                });
                if (stopping_.load()) break;
            }
            if (!source.pull(picture.data(), err)) {
                std::lock_guard<std::mutex> lk(ringMutex);
                producerError = err;
                break;
            }
            const std::uint64_t captureMs =
                std::uint64_t(t0Ms) + std::uint64_t(double(k) * frameSeconds * 1000.0 + 0.5);
            const auto bytes = composer.compose(image, k, captureMs);
            std::memcpy(ring[k % kRingDepth].data(), bytes.data(), frameBytes);
            audioOverflow.store(composer.audio().stats().overflowedPackets);
            {
                std::lock_guard<std::mutex> lk(ringMutex);
                produced = k + 1;
            }
            ringCv.notify_all();
        }
        std::lock_guard<std::mutex> lk(ringMutex);
        producerDone = true;
        ringCv.notify_all();
    });

    auto finish = [&](const std::string& error) {
        stopping_.store(true);
        ringCv.notify_all();
        producer.join();
        if (!error.empty()) fail(error);
    };

    {
        std::unique_lock<std::mutex> lk(ringMutex);
        ringCv.wait(lk, [&] {
            return producerDone || stopping_.load() || produced + 1 >= std::uint64_t(kRingDepth);
        });
        if (producerDone) {
            const std::string e = producerError.empty() ? "picture source ended" : producerError;
            lk.unlock();
            finish(e);
            return;
        }
    }
    if (stopping_.load()) { finish(""); running_.store(false); return; }

    // ---- sockets ------------------------------------------------------------
    MulticastSender txA, txB;
    if (!txA.open({cfg.pathA.group, cfg.pathA.port, cfg.pathA.interfaceIp}, cfg.ttl, cfg.loopback)) {
        finish("path A: " + txA.lastError());
        return;
    }
    txA.enableSegmentation(int(kDatagramBytes));
    const bool haveB = cfg.enablePathB;
    if (haveB) {
        if (!txB.open({cfg.pathB.group, cfg.pathB.port, cfg.pathB.interfaceIp}, cfg.ttl,
                      cfg.loopback)) {
            finish("path B: " + txB.lastError());
            return;
        }
        txB.enableSegmentation(int(kDatagramBytes));
    }
    std::string warning = txA.bufferShortfall();
    if (warning.empty() && haveB) warning = txB.bufferShortfall();

    // ---- transmit -----------------------------------------------------------
    HbrmtHeader hb = hbrmtForFormat(fi);
    hb.r = std::uint8_t(cfg.ref);
    const std::int64_t frameNs = std::int64_t(grid.frameStartNs(1'000'000) / 1'000'000);

    // Frame 0 leaves when its marker says it does; the transmit loop spins the
    // last stretch onto the exact grid instant.
    std::this_thread::sleep_until(t0 - std::chrono::milliseconds(2));
    const double startLateMs =
        std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    if (startLateMs > 1000.0 * frameSeconds && warning.empty())
        warning = "started " + std::to_string(int(startLateMs)) +
                  " ms late: marker capture times run ahead of the wire by that much";

    {
        std::lock_guard<std::mutex> lk(mutex_);
        status_.running = true;
        status_.warning = warning;
        status_.formatText = formatDescription(fi.id);
        status_.targetPps = pps;
        status_.runTag = cfg.composer.runTag;
        status_.taiOffsetS = grid.taiOffsetS();
        status_.taiOffsetFromKernel = kernelTaiOffset != 0;
        auto describe = [](const SenderPath& p) {
            std::string s = p.group + ":" + std::to_string(p.port);
            if (!p.interfaceIp.empty()) s += "  via " + p.interfaceIp;
            return s;
        };
        status_.destinationA = describe(cfg.pathA);
        status_.destinationB = haveB ? describe(cfg.pathB) : std::string();
    }

    SpinPacer::elevateCurrentThread();

    // ---- per-frame pacing on the ST 2059-1 grid -----------------------------
    // Every frame starts at its own grid instant, mapped onto CLOCK_MONOTONIC
    // afresh from the system clock, and its datagrams are spread evenly across
    // its period from that instant. A late or stalled frame therefore never
    // carries its lateness into the next one, and a frame whose whole slot has
    // passed is skipped rather than sent late: a hiccup costs frames, never
    // the phase against the reference. A continuous packet schedule anchored
    // once at start-up let frame starts wander by milliseconds.
    auto monoNs = [] {
        timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return std::int64_t(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
    };
    auto monoOfTai = [&](std::uint64_t taiNs) {
        const std::int64_t m0 = monoNs();
        const std::int64_t tai = std::int64_t(grid.taiFromUtcNs(St2059Grid::utcNowNs()));
        const std::int64_t m1 = monoNs();
        return (m0 + m1) / 2 + (std::int64_t(taiNs) - tai);
    };
    auto waitUntil = [&](std::int64_t deadline) {
        constexpr std::int64_t kSpinNs = 150'000;   // sleep to within this, then spin
        std::int64_t now = monoNs();
        if (deadline - now > kSpinNs) {
            const std::int64_t wake = deadline - kSpinNs;
            timespec until{time_t(wake / 1'000'000'000), long(wake % 1'000'000'000)};
            while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &until, nullptr) == EINTR) {}
        }
        while (monoNs() < deadline) __builtin_ia32_pause();
    };

    const int burstMax = MulticastSender::maxSegments(int(kDatagramBytes));
    std::vector<std::uint8_t> batch(std::size_t(burstMax) * kDatagramBytes);
    const std::uint8_t* frame = nullptr;
    std::uint64_t gridIndex = firstGridIndex;
    std::uint64_t framesSent = 0, sent = 0, repeats = 0, framesTaken = 0;
    std::uint64_t realigns = 0, skippedFrames = 0;
    std::int64_t maxStartLateNs = 0;
    const auto runStart = std::chrono::steady_clock::now();
    std::uint16_t seq = 0;
    bool completed = false;
    std::string failure;
    double lastPublish = 0.0;
    std::uint64_t lastSent = 0;
    const TimecodeRate tcRate = timecodeRate(fi.frameRateNum, fi.frameRateDen);

    while (!stopping_.load(std::memory_order_relaxed)) {
        const std::int64_t periodNs =
            std::int64_t(grid.frameStartNs(gridIndex + 1) - grid.frameStartNs(gridIndex));
        const std::int64_t startMono = monoOfTai(grid.frameStartNs(gridIndex));
        const std::int64_t lateNs = monoNs() - startMono;
        if (lateNs > periodNs || lateNs < -2 * frameNs) {
            // The slot has gone (or the clock stepped back): move to the frame
            // that is due next. The marker and timecode on the picture carry on
            // from the frame sent last.
            const std::uint64_t next =
                grid.indexAtOrAfter(grid.taiFromUtcNs(St2059Grid::utcNowNs()) + 1'000'000);
            if (next > gridIndex) skippedFrames += next - gridIndex;
            gridIndex = next;
            ++realigns;
            continue;
        }

        {
            bool advanced = false;
            {
                std::lock_guard<std::mutex> lk(ringMutex);
                if (produced > taken) {
                    frame = ring[taken % kRingDepth].data();
                    ++taken;
                    advanced = true;
                }
            }
            if (advanced) {
                ++framesTaken;
                ringCv.notify_all();
            } else {
                ++repeats;
            }
        }
        if (!frame) {   // nothing composed yet: hold this slot empty
            ++gridIndex;
            continue;
        }

        // FRCount follows the grid, so a skipped slot reads as a gap of the
        // same size in both FRCount and the RTP timestamp.
        hb.frCount = std::uint8_t(gridIndex);
        const std::uint32_t frameRtp = grid.rtpTimestamp(gridIndex);
        const std::uint32_t frameVideo = grid.videoTimestamp(gridIndex);

        std::uint64_t i = 0;
        bool firstBurst = true;
        while (i < dgPerFrame) {
            const std::int64_t due = startMono + std::int64_t((unsigned __int128)i * std::uint64_t(periodNs) / dgPerFrame);
            waitUntil(due);
            const std::int64_t now = monoNs();
            if (firstBurst) {
                maxStartLateNs = std::max(maxStartLateNs, now - startMono);
                firstBurst = false;
            }
            // Every datagram due by now, up to one send's worth.
            std::uint64_t owed = std::uint64_t(
                (unsigned __int128)std::uint64_t(std::max<std::int64_t>(0, now - startMono)) * dgPerFrame /
                std::uint64_t(periodNs)) + 1;
            owed = std::min<std::uint64_t>(owed, dgPerFrame);
            const int count = int(std::clamp<std::uint64_t>(owed > i ? owed - i : 1, 1, std::uint64_t(burstMax)));

            for (int b = 0; b < count; ++b) {
                const std::uint64_t idx = i + std::uint64_t(b);
                RtpHeader rtp;
                rtp.sequence  = seq++;
                // Per datagram, by its bit position in the frame (ST 2022-6).
                const std::uint64_t bitOffset = idx * kHbrmtPayloadBytes * 8;
                rtp.timestamp = frameRtp + grid.rtpOffset(bitOffset, frameBits);
                rtp.ssrc      = cfg.ssrc;
                rtp.marker    = (idx + 1 == dgPerFrame);
                hb.videoTimestamp = frameVideo + St2059Grid::videoWordOffset(bitOffset);

                // Frame-aligned, the tail datagram zero-padded, as PCAP Replay sends.
                std::uint8_t chunk[kHbrmtPayloadBytes];
                const std::size_t within = std::size_t(idx) * kHbrmtPayloadBytes;
                const std::size_t take = std::min(kHbrmtPayloadBytes, frameBytes - within);
                std::memcpy(chunk, frame + within, take);
                if (take < kHbrmtPayloadBytes) std::memset(chunk + take, 0, kHbrmtPayloadBytes - take);
                buildDatagram(rtp, hb, {chunk, kHbrmtPayloadBytes},
                              batch.data() + std::size_t(b) * kDatagramBytes, kDatagramBytes);
            }
            // A and B leave together.
            txA.sendMany(batch.data(), int(kDatagramBytes), count);
            if (haveB) txB.sendMany(batch.data(), int(kDatagramBytes), count);
            sent += std::uint64_t(count);
            i += std::uint64_t(count);
        }
        ++framesSent;
        ++gridIndex;

        const double elapsedSeconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - runStart).count();
        if (cfg.maxSeconds > 0.0 && elapsedSeconds >= cfg.maxSeconds) {
            completed = true;
            break;
        }
        if (elapsedSeconds - lastPublish >= 0.25) {
            bool producerFailed = false;
            {
                std::lock_guard<std::mutex> lk(ringMutex);
                if (producerDone && !producerError.empty()) {
                    producerFailed = true;
                    failure = producerError;
                }
            }
            if (producerFailed) break;

            const double dt = elapsedSeconds - lastPublish;
            std::lock_guard<std::mutex> lk(mutex_);
            status_.framesSent = framesSent;
            status_.repeatedFrames = repeats;
            status_.elapsedSeconds = elapsedSeconds;
            status_.gridRealigns = realigns;
            status_.skippedFrames = skippedFrames;
            status_.datagrams = sent;
            status_.achievedPps = double(sent - lastSent) / dt;
            status_.wireMbps = status_.achievedPps * double(kDatagramBytes + 28) * 8.0 / 1e6;
            status_.maxLatenessUs = double(maxStartLateNs) / 1000.0;
            status_.audioOverflowPackets = audioOverflow.load();
            status_.timecode = framesTaken
                ? timecodeText(timecodeFromFrames(
                      cfg.composer.timecodeStartFrames + std::int64_t(framesTaken - 1), tcRate))
                : std::string();
            lastPublish = elapsedSeconds;
            lastSent = sent;
        }
    }

    finish(failure);
    std::lock_guard<std::mutex> lk(mutex_);
    status_.running = false;
    status_.completed = completed;
    running_.store(false, std::memory_order_relaxed);
}

}  // namespace testsignal
