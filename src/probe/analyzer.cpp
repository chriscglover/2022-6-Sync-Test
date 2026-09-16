#include "probe/analyzer.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>

namespace testsignal {

// ---- Analyzer ---------------------------------------------------------------

Analyzer::Analyzer(Region fixedRegion) : region_(fixedRegion), fixed_(fixedRegion.valid()) {}

void Analyzer::setSourceRaster(int width, int height) {
    if (width > 0 && height > 0) {
        sourceW_ = width;
        sourceH_ = height;
    }
}

Observation Analyzer::analyse(const LumaFrame& f) {
    Observation o;
    o.arrivalNs = f.arrivalNs;
    o.framePeriodNs = f.fpsNum > 0 ? 1e9 * double(f.fpsDen) / double(f.fpsNum) : 0.0;
    if (f.width <= 0 || f.height <= 0 || f.luma.size() < std::size_t(f.width) * std::size_t(f.height))
        return o;

    Region r = region_.valid() ? region_ : Region{0, 0, f.width, f.height};
    r.w = std::min(r.w, f.width - r.x);
    r.h = std::min(r.h, f.height - r.y);

    // Every third row, so both fields of an interlaced frame are sampled. A
    // flash that starts on a field boundary other than the frame's lights only
    // the later field of one frame and the earlier field of the next; judged on
    // one field, or on the whole frame, it would be dated a field wrong.
    std::uint64_t sum[2] = {0, 0}, count[2] = {0, 0};
    for (int y = r.y; y < r.y + r.h; y += 3) {
        const std::uint8_t* row = f.luma.data() + std::size_t(y) * std::size_t(f.width);
        for (int x = r.x; x < r.x + r.w; x += 4) { sum[y & 1] += row[x]; ++count[y & 1]; }
    }
    const auto mean = [&](int parity) {
        return count[parity] ? double(sum[parity]) / double(count[parity]) : 0.0;
    };
    const std::uint64_t allCount = count[0] + count[1];
    o.meanLuma = allCount ? double(sum[0] + sum[1]) / double(allCount) : 0.0;
    if (baseline_ < 0.0) baseline_ = o.meanLuma;
    const double threshold = std::max(90.0, baseline_ + 60.0);
    if (f.interlaced) {
        const int earlier = f.bottomFieldFirst ? 1 : 0;
        const bool earlyBright = mean(earlier) > threshold;
        const bool lateBright = mean(1 - earlier) > threshold;
        o.flash = earlyBright || lateBright;
        if (earlyBright && !prevFlash_) {
            o.flashStart = true;
            o.flashNs = f.arrivalNs;
        } else if (!earlyBright && lateBright) {
            o.flashStart = true;
            o.flashNs = f.arrivalNs + std::int64_t(std::llround(o.framePeriodNs / 2.0));
        }
        prevFlash_ = lateBright;
    } else {
        o.flash = o.meanLuma > threshold;
        o.flashStart = o.flash && !prevFlash_;
        if (o.flashStart) o.flashNs = f.arrivalNs;
        prevFlash_ = o.flash;
    }
    if (!o.flash) baseline_ = baseline_ * 0.95 + o.meanLuma * 0.05;

    // Locate the test picture from the first flash.
    if (!fixed_ && !calibrated_) {
        if (o.flashStart && prevW_ == f.width && prevH_ == f.height) {
            // Count, per column and per row, the samples that jumped to bright.
            // The test picture flashes across nearly its whole height and width
            // (only its text boxes and marker stay dark); furniture beside it --
            // meters, labels -- may change on the same frame but only in part of
            // a column or row. Keep columns and rows with at least half the
            // busiest one's count, so a bounding box cannot grow into them.
            std::vector<int> columns(std::size_t(f.width), 0), rows(std::size_t(f.height), 0);
            // Every row: a flash starting mid-frame changes only one field.
            for (int y = 0; y < f.height; ++y) {
                const std::uint8_t* cur = f.luma.data() + std::size_t(y) * std::size_t(f.width);
                const std::uint8_t* old = prev_.data() + std::size_t(y) * std::size_t(f.width);
                for (int x = 0; x < f.width; x += 2) {
                    if (int(cur[x]) - int(old[x]) > 80) {
                        ++columns[std::size_t(x)];
                        ++rows[std::size_t(y)];
                    }
                }
            }
            auto span = [](const std::vector<int>& counts, int& first, int& last) {
                const int peak = *std::max_element(counts.begin(), counts.end());
                first = -1;
                last = -1;
                if (peak <= 0) return;
                for (int i = 0; i < int(counts.size()); ++i) {
                    if (counts[std::size_t(i)] * 2 >= peak) {
                        if (first < 0) first = i;
                        last = i;
                    }
                }
            };
            int x0, x1, y0, y1;
            span(columns, x0, x1);
            span(rows, y0, y1);
            y0 &= ~1;
            y1 |= 1;
            if (x0 >= 0 && y0 >= 0 && x1 > x0 && y1 > y0) {
                // Round the sampled edges out to the 2-pixel grid they came from.
                Region found{x0, y0, std::min(f.width, x1 + 2) - x0, std::min(f.height, y1 + 2) - y0};
                if (std::int64_t(found.w) * found.h * 25 >= std::int64_t(f.width) * f.height) {
                    region_ = found;
                    calibrated_ = true;
                }
            }
        }
        if (!calibrated_) {
            prev_.assign(f.luma.begin(), f.luma.end());
            prevW_ = f.width;
            prevH_ = f.height;
        } else {
            prev_.clear();
            prev_.shrink_to_fit();
        }
    }

    MarkerBox box = region_.valid()
        ? scaledMarkerBox(sourceW_, sourceH_, region_.x, region_.y, region_.w, region_.h)
        : nativeMarkerBox(f.width, f.height);
    const MarkerRead m = decodeMarker(f.luma.data(), f.width, f.height, box);
    if (m.ok) {
        o.frameNumber = m.frameNumber;
        o.runTag = m.runTag;
    }
    return o;
}

// An Observation built without a flash time (a test's, say) flashed as its frame arrived.
static std::int64_t flashTime(const Observation& o) {
    return o.flashNs ? o.flashNs : o.arrivalNs;
}

// ---- MuteDetector -----------------------------------------------------------

namespace {
constexpr double kLoudRms = 0.01;     // -40 dBFS: the tone is present
constexpr double kSilentRms = 0.001;  // -60 dBFS: it has stopped
constexpr float  kSilentSample = 1e-4f;
}  // namespace

void MuteDetector::feed(const AudioBlock& block, std::vector<std::int64_t>& muteStarts) {
    if (block.samples.empty() || block.rate <= 0) return;
    const double sampleNs = 1e9 / double(block.rate);
    // A gap or overlap of more than 2 ms means the blocks are not contiguous.
    const double expected = pendingStartNs_ + double(pending_.size()) * sampleNs;
    if (pending_.empty() || block.rate != rate_ || std::fabs(double(block.startNs) - expected) > 2e6) {
        pending_.clear();
        pendingStartNs_ = double(block.startNs);
        rate_ = block.rate;
    }
    pending_.insert(pending_.end(), block.samples.begin(), block.samples.end());

    const std::size_t window = std::size_t(std::max(1, rate_ / 1000));
    std::size_t used = 0;
    while (pending_.size() - used >= window) {
        const float* w = pending_.data() + used;
        double energy = 0.0;
        for (std::size_t i = 0; i < window; ++i) energy += double(w[i]) * double(w[i]);
        const double rms = std::sqrt(energy / double(window));
        const double startNs = pendingStartNs_ + double(used) * sampleNs;
        if (rms > kLoudRms) {
            loud_ = true;
            toneSeen_ = true;
            lastLoudRms_ = rms;
        } else if (rms < kSilentRms && loud_) {
            // Refine: the silence may have begun inside the previous window.
            int back = 0;
            for (auto it = lastWindow_.rbegin(); it != lastWindow_.rend() && std::fabs(*it) < kSilentSample; ++it)
                ++back;
            muteStarts.push_back(std::int64_t(std::llround(startNs - double(back) * sampleNs)));
            loud_ = false;
        }
        lastWindow_.assign(w, w + window);
        used += window;
    }
    pending_.erase(pending_.begin(), pending_.begin() + std::ptrdiff_t(used));
    pendingStartNs_ += double(used) * sampleNs;
}

double MuteDetector::toneDbfs() const {
    return lastLoudRms_ > 0.0 ? 20.0 * std::log10(lastLoudRms_ * std::sqrt(2.0)) : -200.0;
}

// ---- Correlator -------------------------------------------------------------

void DelayStats::add(double ms) {
    if (n == 0) { minMs = maxMs = ms; }
    minMs = std::min(minMs, ms);
    maxMs = std::max(maxMs, ms);
    sumMs += ms;
    ++n;
}

Correlator::Correlator(std::string reference, std::FILE* csv)
    : reference_(std::move(reference)), csv_(csv) {
    if (csv_) std::fprintf(csv_, "source,event,time_ns,frame_number,delay_ms,flash_delay_ms\n");
}

void Correlator::observe(const std::string& source, const Observation& o, double offsetFrames) {
    std::lock_guard<std::mutex> lk(mutex_);
    double delayMs = 0.0, flashDelayMs = 0.0;
    bool haveDelay = false, haveFlashDelay = false;

    SourceState& s = sources_[source];
    s.offsetFrames = offsetFrames;
    if (o.framePeriodNs > 0) s.periodNs = o.framePeriodNs;
    const double offsetNs = offsetFrames * s.periodNs;

    if (source == reference_) {
        if (o.frameNumber >= 0) {
            const auto fn = std::uint32_t(o.frameNumber);
            if (o.runTag != refRunTag_) {
                refRunTag_ = o.runTag;
                refArrival_.clear();
                refOrder_.clear();
            }
            if (refArrival_.emplace(fn, o.arrivalNs).second) {
                refOrder_.push_back(fn);
                while (refOrder_.size() > 4096) {
                    refArrival_.erase(refOrder_.front());
                    refOrder_.pop_front();
                }
            }
        }
    } else if (!reference_.empty()) {
        if (o.frameNumber < 0) {
            ++s.framesNoMarker;
        } else if (o.frameNumber != s.lastFrame) {
            // First frame on this source carrying this frame number.
            s.lastFrame = o.frameNumber;
            auto it = refArrival_.find(std::uint32_t(o.frameNumber));
            if (it != refArrival_.end() && o.runTag == refRunTag_) {
                delayMs = (double(o.arrivalNs - it->second) - offsetNs) / 1e6;
                haveDelay = true;
                s.interval.add(delayMs);
                s.total.add(delayMs);
            } else {
                ++s.framesUnmatched;
            }
        }
        if (o.flashStart) {
            const std::int64_t flashAt = flashTime(o);
            // The most recent reference flash at or before this one, within 2 s.
            const auto ref = sources_.find(reference_);
            if (ref != sources_.end()) {
                for (auto it = ref->second.flashes.rbegin(); it != ref->second.flashes.rend(); ++it) {
                    if (*it <= flashAt && flashAt - *it < 2'000'000'000) {
                        flashDelayMs = (double(flashAt - *it) - offsetNs) / 1e6;
                        haveFlashDelay = true;
                        char buf[64];
                        std::snprintf(buf, sizeof buf, "%.1f ms", flashDelayMs);
                        s.lastFlashDelay = buf;
                        break;
                    }
                }
            }
        }
    }
    if (o.flashStart) {
        s.flashes.push_back(flashTime(o));
        while (s.flashes.size() > 16) s.flashes.pop_front();
    }

    if (csv_) {
        std::fprintf(csv_, "%s,%s,%" PRId64 ",%" PRId64 ",", source.c_str(), o.flashStart ? "flash" : "frame",
                     o.flashStart ? flashTime(o) : o.arrivalNs, o.frameNumber);
        if (haveDelay) std::fprintf(csv_, "%.3f", delayMs);
        std::fputc(',', csv_);
        if (haveFlashDelay) std::fprintf(csv_, "%.3f", flashDelayMs);
        std::fputc('\n', csv_);
    }
}

void Correlator::observeMute(const std::string& source, std::int64_t muteNs) {
    std::lock_guard<std::mutex> lk(mutex_);
    SourceState& s = sources_[source];
    s.mutes.push_back(muteNs);
    while (s.mutes.size() > 32) s.mutes.pop_front();
    if (csv_) std::fprintf(csv_, "%s,mute,%" PRId64 ",,,\n", source.c_str(), muteNs);
}

void Correlator::noteAudio(const std::string& source, double toneDbfs) {
    std::lock_guard<std::mutex> lk(mutex_);
    SourceState& s = sources_[source];
    s.audio = true;
    s.toneDbfs = toneDbfs;
}

void Correlator::pairMutes(SourceState& s, std::int64_t now) {
    // A mute is paired once a flash up to 1 s after it would already have
    // arrived, so an audio-early source is measured as well as an audio-late one.
    while (!s.mutes.empty() && now - s.mutes.front() > 1'200'000'000) {
        const std::int64_t mute = s.mutes.front();
        s.mutes.pop_front();
        std::int64_t best = 0;
        std::int64_t bestAbs = 1'000'000'000;
        for (const std::int64_t flash : s.flashes) {
            const std::int64_t d = mute - flash;
            if (std::llabs(d) < bestAbs) { bestAbs = std::llabs(d); best = d; }
        }
        if (bestAbs < 1'000'000'000) {
            const double ms = double(best) / 1e6;
            s.lipInterval.add(ms);
            s.lipTotal.add(ms);
        }
    }
}

std::vector<std::string> Correlator::report() {
    std::lock_guard<std::mutex> lk(mutex_);
    const std::int64_t now = monotonicNs();
    std::vector<std::string> lines;
    for (auto& [name, s] : sources_) {
        pairMutes(s, now);
        std::string line = name;
        line.resize(std::max<std::size_t>(line.size(), 8), ' ');
        char buf[320];
        if (!reference_.empty() && name != reference_) {
            if (s.interval.n)
                std::snprintf(buf, sizeof buf,
                              " delay %7.2f ms  (min %.2f  max %.2f, %llu frames)  overall %.2f ms"
                              "  | flash %s | offset %+.1f frames | no marker %llu, unmatched %llu",
                              s.interval.mean(), s.interval.minMs, s.interval.maxMs,
                              static_cast<unsigned long long>(s.interval.n), s.total.mean(),
                              s.lastFlashDelay.empty() ? "-" : s.lastFlashDelay.c_str(), s.offsetFrames,
                              static_cast<unsigned long long>(s.framesNoMarker),
                              static_cast<unsigned long long>(s.framesUnmatched));
            else
                std::snprintf(buf, sizeof buf, " no matched frames this interval | flash %s | no marker %llu, unmatched %llu",
                              s.lastFlashDelay.empty() ? "-" : s.lastFlashDelay.c_str(),
                              static_cast<unsigned long long>(s.framesNoMarker),
                              static_cast<unsigned long long>(s.framesUnmatched));
            line += buf;
            line += "\n        ";
        }
        if (s.lipInterval.n)
            std::snprintf(buf, sizeof buf, " lip sync %+.1f ms, audio %s (min %+.1f  max %+.1f, %llu flashes)  overall %+.1f ms",
                          s.lipInterval.mean(), s.lipInterval.mean() >= 0 ? "late" : "early",
                          s.lipInterval.minMs, s.lipInterval.maxMs,
                          static_cast<unsigned long long>(s.lipInterval.n), s.lipTotal.mean());
        else if (s.lipTotal.n)
            std::snprintf(buf, sizeof buf, " lip sync overall %+.1f ms (%llu flashes; none this interval)",
                          s.lipTotal.mean(), static_cast<unsigned long long>(s.lipTotal.n));
        else if (s.audio)
            std::snprintf(buf, sizeof buf, " lip sync: tone heard at %.1f dBFS, waiting for a flash and its mute", s.toneDbfs);
        else
            std::snprintf(buf, sizeof buf, " lip sync: no tone");
        line += buf;
        lines.push_back(line);
        s.interval = DelayStats{};
        s.lipInterval = DelayStats{};
    }
    if (csv_) std::fflush(csv_);
    return lines;
}

}  // namespace testsignal
