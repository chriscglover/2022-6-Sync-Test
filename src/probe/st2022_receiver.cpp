#include "probe/st2022_receiver.h"

#include <algorithm>
#include <cstring>
#include <ctime>

#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "composer.h"
#include "pcapreplay/bitpack.h"
#include "pcapreplay/hbrmt.h"
#include "pcapreplay/sdi_format.h"
#include "pcapreplay/sdi_raster.h"

namespace testsignal {

using namespace pcapreplay;

namespace {

// One leg's receive socket.
//
// Bound to the group address, not INADDR_ANY, and with IP_MULTICAST_ALL off:
// otherwise Linux delivers every group any process on this host has joined on
// the same port, and a host receiving several ST 2022-6 streams usually has
// them all on one port -- unrelated streams would be merged into this one.
class GroupSocket {
public:
    GroupSocket() = default;
    ~GroupSocket() { if (fd_ >= 0) ::close(fd_); }
    GroupSocket(const GroupSocket&) = delete;
    GroupSocket& operator=(const GroupSocket&) = delete;

    bool open(const St2022Leg& leg, std::string& error) {
        in_addr group{}, iface{};
        if (inet_pton(AF_INET, leg.group.c_str(), &group) != 1) {
            error = "invalid multicast group '" + leg.group + "'";
            return false;
        }
        if (!leg.interfaceIp.empty() && inet_pton(AF_INET, leg.interfaceIp.c_str(), &iface) != 1) {
            error = "invalid interface address '" + leg.interfaceIp + "'";
            return false;
        }
        fd_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (fd_ < 0) { error = std::string("socket: ") + std::strerror(errno); return false; }
        const int one = 1, zero = 0;
        setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = htons(leg.port);
        local.sin_addr = group;
        if (bind(fd_, reinterpret_cast<sockaddr*>(&local), sizeof local) != 0) {
            error = std::string("bind: ") + std::strerror(errno);
            return false;
        }
        setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_ALL, &zero, sizeof zero);
        const int buffer = 32 * 1024 * 1024;
        setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof buffer);
        const timeval timeout{0, 100000};
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
        ip_mreq mreq{};
        mreq.imr_multiaddr = group;
        mreq.imr_interface = iface;
        if (setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof mreq) != 0) {
            error = std::string("IP_ADD_MEMBERSHIP: ") + std::strerror(errno);
            return false;
        }
        return true;
    }

    // Up to `max` datagrams; blocks for the first (100 ms timeout) only.
    int receive(std::uint8_t* buffer, std::size_t stride, int* lengths, int max) {
        mmsghdr messages[64];
        iovec vectors[64];
        max = std::min(max, 64);
        for (int i = 0; i < max; ++i) {
            vectors[i] = {buffer + std::size_t(i) * stride, stride};
            messages[i] = {};
            messages[i].msg_hdr.msg_iov = &vectors[i];
            messages[i].msg_hdr.msg_iovlen = 1;
        }
        const int n = recvmmsg(fd_, messages, unsigned(max), MSG_WAITFORONE, nullptr);
        if (n <= 0) return 0;
        for (int i = 0; i < n; ++i) lengths[i] = int(messages[i].msg_len);
        return n;
    }

private:
    int fd_ = -1;
};

const SdiFormatInfo* frameFormat(const PackedFrame& packed) {
    const SdiFormat format = formatFromHbrmtCodes(packed.frameCode, packed.frateCode, packed.sampleCode);
    if (format == SdiFormat::Unknown) return nullptr;
    const SdiFormatInfo& fi = formatInfo(format);
    if (std::int64_t(packed.bytes.size()) < fi.bytesPerFrame()) return nullptr;
    return &fi;
}

}  // namespace

std::int64_t monotonicNs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return std::int64_t(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

// ---- merge ------------------------------------------------------------------

St2022Merger::St2022Merger(PackedSink sink, int window)
    : sink_(std::move(sink)), slots_(std::size_t(window)), window_(std::uint64_t(window)) {}

bool St2022Merger::push(const std::uint8_t* data, std::size_t len, std::int64_t arrivalNs) {
    ParsedDatagram pd{};
    if (!parseDatagram({data, len}, pd) || pd.payload.size() != kHbrmtPayloadBytes) return false;
    // One stream only: both -7 legs carry the same SSRC; anything else is a
    // different sender and must never enter the sequence merge.
    if (!haveSsrc_) {
        haveSsrc_ = true;
        ssrc_ = pd.rtp.ssrc;
    } else if (pd.rtp.ssrc != ssrc_) {
        ++foreign_;
        return false;
    }

    const std::uint16_t seq = pd.rtp.sequence;
    std::uint64_t ext;
    if (!haveSeq_) {
        haveSeq_ = true;
        // Headroom below for early reordering; the low 16 bits must be the RTP
        // sequence itself, because later packets are unwrapped against them.
        ext = (std::uint64_t(1) << 32) | seq;
        highest_ = next_ = ext;
    } else {
        const std::int16_t d = std::int16_t(std::uint16_t(seq - std::uint16_t(highest_)));
        ext = std::uint64_t(std::int64_t(highest_) + d);
        if (ext > highest_) highest_ = ext;
    }
    if (ext < next_) { ++duplicates_; return true; }

    // Keep the window: anything that would fall off the far end is resolved now.
    while (ext - next_ >= window_) advance();

    Slot& s = slots_[ext % window_];
    if (s.present && s.ext == ext) { ++duplicates_; return true; }
    s.present = true;
    s.ext = ext;
    s.marker = pd.rtp.marker;
    s.arrivalNs = arrivalNs;
    s.frameCode = pd.hbrmt.frame;
    s.frateCode = pd.hbrmt.frate;
    s.sampleCode = pd.hbrmt.sample;
    std::memcpy(s.payload, pd.payload.data(), kHbrmtPayloadBytes);

    // Emit everything contiguous; give up on a hole once the stream is well past
    // it (half the window, several thousand packets: far beyond any -7 skew).
    while (next_ <= highest_) {
        const Slot& n = slots_[next_ % window_];
        if (n.present && n.ext == next_) { advance(); continue; }
        if (highest_ - next_ < window_ / 2) break;
        advance();
    }
    return true;
}

void St2022Merger::advance() {
    Slot& s = slots_[next_ % window_];
    if (s.present && s.ext == next_) {
        emit(s);
        s.present = false;
    } else {
        ++missing_;
        if (frameStarted_) frame_.complete = false;
    }
    ++next_;
}

void St2022Merger::emit(const Slot& s) {
    if (!synced_) {
        // Start at a frame boundary: the datagram after a marker.
        if (s.marker) synced_ = true;
        return;
    }
    if (!frameStarted_) {
        frameStarted_ = true;
        frame_.bytes.clear();
        frame_.arrivalNs = s.arrivalNs;
        frame_.frameCode = s.frameCode;
        frame_.frateCode = s.frateCode;
        frame_.sampleCode = s.sampleCode;
        frame_.complete = true;
    }
    frame_.arrivalNs = std::min(frame_.arrivalNs, s.arrivalNs);
    frame_.bytes.insert(frame_.bytes.end(), s.payload, s.payload + kHbrmtPayloadBytes);
    if (s.marker) {
        PackedFrame done;
        std::swap(done, frame_);
        frameStarted_ = false;
        sink_(std::move(done));
    }
}

// ---- luma and audio ---------------------------------------------------------

bool extractLuma(const PackedFrame& packed, LumaFrame& out, std::string& problem) {
    const SdiFormatInfo* fip = frameFormat(packed);
    if (!fip) { problem = "unknown format, or frame shorter than its raster"; return false; }
    const SdiFormatInfo& fi = *fip;
    if (!packed.complete) { problem = "frame has datagrams neither leg delivered"; return false; }

    std::uint16_t head[8];
    unpack10(packed.bytes.data(), 8, head);
    const bool eavFirst = fi.isHd
        ? (head[0] == 0x3FF && head[1] == 0x3FF && head[2] == 0 && head[3] == 0)
        : (head[0] == 0x3FF && head[1] == 0 && head[2] == 0);
    if (!eavFirst) { problem = "frame does not start with line 1's EAV"; return false; }

    const int lw = fi.totalSamples * 2;
    const int aw = fi.activeWidth * 2;
    out.width = fi.activeWidth;
    out.height = fi.activeHeight;
    out.luma.assign(std::size_t(out.width) * std::size_t(out.height), 16);
    out.arrivalNs = packed.arrivalNs;
    out.fpsNum = fi.frameRateNum;
    out.fpsDen = fi.frameRateDen;
    out.interlaced = fi.interlacedOnWire();
    out.bottomFieldFirst = out.interlaced && fi.totalLines == 525;
    out.format = fi.name;

    std::vector<std::uint16_t> words(static_cast<std::size_t>(aw));
    for (int line = 1; line <= fi.totalLines; ++line) {
        const WireLine wl = wireLine(fi, line);
        if (wl.activeRow < 0 || wl.activeRow >= fi.activeHeight) continue;
        const std::size_t wordOffset = std::size_t(line - 1) * std::size_t(lw) + std::size_t(lw - aw);
        unpack10(packed.bytes.data() + wordOffset / 4 * 5, std::size_t(aw), words.data());
        std::uint8_t* row = out.luma.data() + std::size_t(wl.activeRow) * std::size_t(out.width);
        for (int x = 0; x < fi.activeWidth; ++x) row[x] = std::uint8_t(words[std::size_t(2 * x + 1)] >> 2);
    }
    return true;
}

bool extractAudio(const PackedFrame& packed, AudioBlock& out) {
    const SdiFormatInfo* fip = frameFormat(packed);
    if (!fip || !packed.complete) return false;
    const SdiFormatInfo& fi = *fip;

    const int lw = fi.totalSamples * 2;
    const int post = postActiveWords(fi);
    const int hanc = lw - fi.activeWidth * 2 - savWords(fi) - post;
    const int stride = fi.isHd ? 2 : 1;   // HD audio rides the colour-difference stream
    out.samples.clear();
    out.startNs = packed.arrivalNs;
    out.rate = 48000;

    std::vector<std::uint16_t> w(static_cast<std::size_t>(hanc));
    for (int line = 1; line <= fi.totalLines; ++line) {
        const std::size_t wordOffset = std::size_t(line - 1) * std::size_t(lw) + std::size_t(post);
        unpack10(packed.bytes.data() + wordOffset / 4 * 5, std::size_t(hanc), w.data());
        for (int k = 0; k + 6 * stride < hanc; k += stride) {
            if (w[std::size_t(k)] != 0x000 || w[std::size_t(k + stride)] != 0x3FF ||
                w[std::size_t(k + 2 * stride)] != 0x3FF)
                continue;
            const int did = w[std::size_t(k + 3 * stride)] & 0xFF;
            const int dc = w[std::size_t(k + 5 * stride)] & 0xFF;
            if (k + (6 + dc) * stride >= hanc) break;
            auto udw = [&](int i) { return w[std::size_t(k + (6 + i) * stride)]; };
            if (fi.isHd && did == 0xE7 && dc >= 18) {
                // ST 299-1: UDW2..5 are channel 1's AES subframe, low byte first.
                std::uint32_t sub = 0;
                for (int b = 0; b < 4; ++b) sub |= std::uint32_t(udw(2 + b) & 0xFF) << (8 * b);
                const std::int32_t sample = std::int32_t(sub << 4) >> 8;
                out.samples.push_back(float(sample) / 8388608.0f);
            } else if (!fi.isHd && did == 0xFF) {
                // ST 272: three words per channel sample; keep channel 1.
                for (int x = 0; x + 2 < dc; x += 3) {
                    const std::uint32_t w0 = udw(x), w1 = udw(x + 1), w2 = udw(x + 2);
                    if (((w0 >> 1) & 3) != 0) continue;
                    std::int32_t s = std::int32_t(((w0 >> 3) & 0x3F) | ((w1 & 0x1FF) << 6) | ((w2 & 0x1F) << 15));
                    if (s & 0x80000) s -= 0x100000;
                    out.samples.push_back(float(s) / 524288.0f);
                }
            }
            k += (6 + dc) * stride;
        }
    }
    return !out.samples.empty();
}

// ---- receiver ---------------------------------------------------------------

St2022Receiver::~St2022Receiver() { stop(); }

bool St2022Receiver::start(const St2022SourceConfig& cfg, FrameSink frames, AudioSink audio,
                           std::string& error) {
    stop();
    frameSink_ = std::move(frames);
    audioSink_ = std::move(audio);
    stopping_.store(false);
    merger_ = std::make_unique<St2022Merger>([this](PackedFrame&& f) {
        std::lock_guard<std::mutex> lk(queueMutex_);
        // Analysis that falls behind drops whole frames rather than let a
        // backlog grow; arrival stamps are already taken.
        if (queue_.size() >= 4) {
            queue_.pop_front();
            std::lock_guard<std::mutex> s(statsMutex_);
            ++stats_.framesDroppedBusy;
        }
        queue_.push_back(std::move(f));
        queueCv_.notify_one();
    });

    // Open both sockets before starting any thread so a bad group fails fast.
    auto probe = [&](const St2022Leg& leg) {
        GroupSocket rx;
        std::string problem;
        if (!rx.open(leg, problem)) {
            error = leg.group + ": " + problem;
            return false;
        }
        return true;
    };
    if (!probe(cfg.a) || (cfg.haveB && !probe(cfg.b))) return false;

    threads_.emplace_back([this] { workerLoop(); });
    threads_.emplace_back([this, leg = cfg.a] { receiveLoop(0, leg); });
    if (cfg.haveB) threads_.emplace_back([this, leg = cfg.b] { receiveLoop(1, leg); });
    return true;
}

void St2022Receiver::stop() {
    stopping_.store(true);
    queueCv_.notify_all();
    for (auto& t : threads_)
        if (t.joinable()) t.join();
    threads_.clear();
    merger_.reset();
    queue_.clear();
}

St2022Stats St2022Receiver::stats() const {
    St2022Stats s;
    {
        std::lock_guard<std::mutex> lk(statsMutex_);
        s = stats_;
    }
    s.datagramsA = datagrams_[0].load();
    s.datagramsB = datagrams_[1].load();
    std::lock_guard<std::mutex> lk(mergeMutex_);
    if (merger_) {
        s.duplicates = merger_->duplicates();
        s.missing = merger_->missing();
        s.foreign = merger_->foreign();
    }
    return s;
}

void St2022Receiver::receiveLoop(int leg, const St2022Leg& cfg) {
    GroupSocket rx;
    std::string problem;
    if (!rx.open(cfg, problem)) {
        std::lock_guard<std::mutex> lk(statsMutex_);
        stats_.lastProblem = cfg.group + ": " + problem;
        return;
    }
    constexpr int kBatch = 64;
    constexpr std::size_t kStride = 1600;
    std::vector<std::uint8_t> buffer(kBatch * kStride);
    int lengths[kBatch];
    while (!stopping_.load(std::memory_order_relaxed)) {
        const int n = rx.receive(buffer.data(), kStride, lengths, kBatch);
        if (n <= 0) continue;
        const std::int64_t now = monotonicNs();
        datagrams_[leg].fetch_add(std::uint64_t(n), std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(mergeMutex_);
        for (int i = 0; i < n; ++i)
            if (lengths[i] > 0) merger_->push(buffer.data() + std::size_t(i) * kStride, std::size_t(lengths[i]), now);
    }
}

void St2022Receiver::workerLoop() {
    LumaFrame luma;
    AudioBlock audio;
    for (;;) {
        PackedFrame packed;
        {
            std::unique_lock<std::mutex> lk(queueMutex_);
            queueCv_.wait(lk, [&] { return stopping_.load() || !queue_.empty(); });
            if (stopping_.load()) return;
            packed = std::move(queue_.front());
            queue_.pop_front();
        }
        std::string problem;
        const bool ok = extractLuma(packed, luma, problem);
        {
            std::lock_guard<std::mutex> lk(statsMutex_);
            if (ok) {
                ++stats_.framesOk;
                stats_.format = luma.format;
            } else {
                ++stats_.framesBad;
                stats_.lastProblem = problem;
            }
        }
        if (!ok) continue;
        frameSink_(luma);
        if (audioSink_ && extractAudio(packed, audio)) audioSink_(audio);
    }
}

}  // namespace testsignal
