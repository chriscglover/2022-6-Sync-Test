// NDI capture through GStreamer's ndisrc, for the delay probe.
//
// Needs the gst-plugins-rs NDI plugin (ndisrc, ndisrcdemux) on GST_PLUGIN_PATH
// and the NDI runtime (NDI_RUNTIME_DIR_V6). The pipeline runs on GStreamer's
// monotonic system clock, as the SDI capture does. Which timestamp a buffer
// carries is ndisrc's `timestamp-mode`: `timestamp` places video and audio by
// the sender's own NDI timestamps (what a timestamp-aware receiver plays out),
// `receive-time` by when this machine received them (including the receiver's
// own audio/video buffering). Lip sync is meaningful in either; delay against
// another source only with `receive-time`.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "probe/probe_types.h"

typedef struct _GstElement GstElement;

namespace testsignal {

struct NdiSourceConfig {
    std::string name;                     // NDI source name, e.g. "HOST (Glovebox Mosaic)"
    std::string timestampMode = "timestamp";
};

struct NdiStats {
    std::uint64_t frames = 0;
    std::uint64_t audioBlocks = 0;
    std::string   format;
    std::string   lastProblem;
};

class NdiCapture {
public:
    NdiCapture() = default;
    ~NdiCapture();
    NdiCapture(const NdiCapture&) = delete;
    NdiCapture& operator=(const NdiCapture&) = delete;

    bool start(const NdiSourceConfig& cfg, FrameSink frames, AudioSink audio, std::string& error);
    void stop();
    NdiStats stats() const;

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
    NdiStats          stats_;
};

}  // namespace testsignal
