// Shared vocabulary for probe: what a source delivers, reduced to
// what the analysis needs, stamped with when it arrived.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace testsignal {

// CLOCK_MONOTONIC in nanoseconds. Every source stamps arrivals on this clock,
// GStreamer included (its system clock is monotonic), so arrivals from an
// ST 2022-6 socket and from a DeckLink capture are directly comparable.
std::int64_t monotonicNs();

struct LumaFrame {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> luma;   // 8-bit Y, width * height
    std::int64_t arrivalNs = 0;       // start of frame, monotonic clock
    int fpsNum = 0;
    int fpsDen = 1;
    bool interlaced = false;
    std::string format;               // e.g. "1080i25 (1080i50)" or "1920x1080p50 v210"
};

// A run of consecutive samples from the first audio channel.
struct AudioBlock {
    std::int64_t startNs = 0;         // time of samples[0], monotonic clock
    int rate = 48000;
    std::vector<float> samples;       // -1.0 .. 1.0
};

using FrameSink = std::function<void(LumaFrame&)>;
using AudioSink = std::function<void(AudioBlock&)>;

}  // namespace testsignal
