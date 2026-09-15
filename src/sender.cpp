#include "sender.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <vector>

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
    const std::uint64_t dgPerFrame =
        (std::uint64_t(frameBytes) + kHbrmtPayloadBytes - 1) / kHbrmtPayloadBytes;
    const double pps = double(dgPerFrame) * fi.frameRate();
    const double frameSeconds = double(fi.frameRateDen) / double(fi.frameRateNum);

    const Clock::time_point t0 = Clock::now() + kPreroll;
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
        if (!source.open(fi.activeWidth, fi.activeHeight, fi.frameRateNum, fi.frameRateDen, err)) {
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
    const double cfHz = hb.cf == 2 ? 148.5e6 : (hb.cf == 1 ? 148.5e6 / 1.001 : 0.0);
    const std::uint32_t videoTicks = cfHz > 0.0 ? std::uint32_t(cfHz / fi.frameRate() + 0.5) : 0;
    const std::uint32_t rtpStep = rtpTicksPerFrame(fi.frameRateNum, fi.frameRateDen);

    // Frame 0 leaves when its marker says it does.
    std::this_thread::sleep_until(t0);
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
        auto describe = [](const SenderPath& p) {
            std::string s = p.group + ":" + std::to_string(p.port);
            if (!p.interfaceIp.empty()) s += "  via " + p.interfaceIp;
            return s;
        };
        status_.destinationA = describe(cfg.pathA);
        status_.destinationB = haveB ? describe(cfg.pathB) : std::string();
    }

    SpinPacer pacer;
    SpinPacer::elevateCurrentThread();
    pacer.start(pps);

    const int burstMax = MulticastSender::maxSegments(int(kDatagramBytes));
    std::vector<std::uint8_t> batch(std::size_t(burstMax) * kDatagramBytes);
    const std::uint8_t* frame = nullptr;
    std::uint64_t streamPos = 0, sent = 0, repeats = 0, framesTaken = 0;
    std::uint16_t seq = 0;
    bool completed = false;
    std::string failure;
    double lastPublish = 0.0;
    std::uint64_t lastSent = 0;
    const TimecodeRate tcRate = timecodeRate(fi.frameRateNum, fi.frameRateDen);

    while (!stopping_.load(std::memory_order_relaxed)) {
        int credit = 0;
        while (credit < burstMax) {
            const int got = pacer.acquire(burstMax - credit);
            if (got <= 0) break;
            credit += got;
        }
        if (credit <= 0) continue;

        int built = 0;
        for (; built < credit; ++built) {
            const std::uint64_t frameIndex = streamPos / dgPerFrame;
            const std::uint64_t idxInFrame = streamPos % dgPerFrame;
            if (idxInFrame == 0) {
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

            RtpHeader rtp;
            rtp.sequence  = seq++;
            rtp.timestamp = std::uint32_t(frameIndex * rtpStep);
            rtp.ssrc      = cfg.ssrc;
            rtp.marker    = (idxInFrame + 1 == dgPerFrame);
            hb.frCount = std::uint8_t(frameIndex);
            hb.videoTimestamp = std::uint32_t(frameIndex * videoTicks);

            // Frame-aligned, the tail datagram zero-padded, as PCAP Replay sends.
            std::uint8_t chunk[kHbrmtPayloadBytes];
            const std::size_t within = std::size_t(idxInFrame) * kHbrmtPayloadBytes;
            const std::size_t take = std::min(kHbrmtPayloadBytes, frameBytes - within);
            std::memcpy(chunk, frame + within, take);
            if (take < kHbrmtPayloadBytes) std::memset(chunk + take, 0, kHbrmtPayloadBytes - take);
            buildDatagram(rtp, hb, {chunk, kHbrmtPayloadBytes},
                          batch.data() + std::size_t(built) * kDatagramBytes, kDatagramBytes);
            ++streamPos;
        }

        // One pacer slot for both legs: A and B leave together.
        txA.sendMany(batch.data(), int(kDatagramBytes), built);
        if (haveB) txB.sendMany(batch.data(), int(kDatagramBytes), built);
        sent += std::uint64_t(built);

        const PacerStats ps = pacer.stats();
        if (cfg.maxSeconds > 0.0 && ps.elapsedSeconds >= cfg.maxSeconds) {
            completed = true;
            break;
        }
        if (ps.elapsedSeconds - lastPublish >= 0.25) {
            bool producerFailed = false;
            {
                std::lock_guard<std::mutex> lk(ringMutex);
                if (producerDone && !producerError.empty()) {
                    producerFailed = true;
                    failure = producerError;
                }
            }
            if (producerFailed) break;

            const double dt = ps.elapsedSeconds - lastPublish;
            std::lock_guard<std::mutex> lk(mutex_);
            status_.framesSent = streamPos / dgPerFrame;
            status_.repeatedFrames = repeats;
            status_.elapsedSeconds = ps.elapsedSeconds;
            status_.datagrams = sent;
            status_.achievedPps = double(sent - lastSent) / dt;
            status_.wireMbps = status_.achievedPps * double(kDatagramBytes + 28) * 8.0 / 1e6;
            status_.maxLatenessUs = ps.maxLatenessUs;
            status_.pacerResyncs = ps.resyncs;
            status_.audioOverflowPackets = audioOverflow.load();
            status_.timecode = framesTaken
                ? timecodeText(timecodeFromFrames(
                      cfg.composer.timecodeStartFrames + std::int64_t(framesTaken - 1), tcRate))
                : std::string();
            lastPublish = ps.elapsedSeconds;
            lastSent = sent;
        }
    }

    pacer.stop();
    finish(failure);
    std::lock_guard<std::mutex> lk(mutex_);
    status_.running = false;
    status_.completed = completed;
    running_.store(false, std::memory_order_relaxed);
}

}  // namespace testsignal
