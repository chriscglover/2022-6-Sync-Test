// SDI capture from a Blackmagic DeckLink through GStreamer, for the delay probe.
//
// The pipeline runs on GStreamer's monotonic system clock, so a buffer's
// capture time (base time + PTS) is on the same clock as the ST 2022-6 receive
// stamps. The DeckLink's own processing delay is not removed here; give the
// source an offset (a DeckLink reports 2 frames).
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "probe/probe_types.h"

typedef struct _GstElement GstElement;

namespace testsignal {

struct SdiSourceConfig {
    int         device = 0;
    std::string mode = "auto";   // DeckLink mode nick, e.g. 1080p50, or auto
};

struct SdiStats {
    std::uint64_t frames = 0;
    std::uint64_t audioBlocks = 0;
    std::string   format;
    std::string   lastProblem;
};

class SdiCapture {
public:
    SdiCapture() = default;
    ~SdiCapture();
    SdiCapture(const SdiCapture&) = delete;
    SdiCapture& operator=(const SdiCapture&) = delete;

    bool start(const SdiSourceConfig& cfg, FrameSink frames, AudioSink audio, std::string& error);
    void stop();
    SdiStats stats() const;

private:
    void pullVideo();
    void pullAudio();

    GstElement*       pipeline_ = nullptr;
    GstElement*       videoSink_ = nullptr;
    GstElement*       audioSinkElement_ = nullptr;
    FrameSink         frameSink_;
    AudioSink         audioSink_;
    std::atomic<bool> stopping_{false};
    std::thread       videoThread_, audioThread_;
    mutable std::mutex mutex_;
    SdiStats          stats_;
};

}  // namespace testsignal
