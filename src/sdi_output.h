// Sends the test signal out of a Blackmagic DeckLink as SDI, or as an NDI
// source.
//
// The picture and the audio for each frame go into GStreamer's decklink sinks
// with matching timestamps: frame k's video buffer and the block of samples
// that belong to frame k start at the same time, so the flash and the tone
// mute leave the card together. The producer blocks on the sinks, so the card's
// clock paces composition; there is no software pacer on this path.
//
// No Blackmagic code is linked: GStreamer's decklink plugin loads
// libDeckLinkAPI.so from Blackmagic Desktop Video at run time.
//
// NDI goes through GStreamer's ndisinkcombiner and ndisink (gst-plugins-rs),
// which load the NDI runtime themselves. There is no card clock there, so the
// sink paces to the pipeline clock; picture and audio still carry the same
// timestamps, so NDI's own per-frame timestamps keep them together.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "composer.h"

typedef struct _GstElement GstElement;

namespace testsignal {

struct SdiOutputConfig {
    ComposerSettings composer;
    bool   timecodeFromTimeOfDay = true;
    int    device = 0;
    std::string ndiName;         // non-empty: send as this NDI source instead of SDI
    double maxSeconds = 0.0;     // 0 = until stopped
};

struct SdiOutputStatus {
    bool          running = false;
    bool          completed = false;
    std::string   error;
    std::string   mode;
    int           audioChannels = 0;
    std::uint64_t framesSent = 0;
    std::string   timecode;
};

class SdiOutput {
public:
    SdiOutput() = default;
    ~SdiOutput();
    SdiOutput(const SdiOutput&) = delete;
    SdiOutput& operator=(const SdiOutput&) = delete;

    bool start(const SdiOutputConfig& cfg, std::string& error);
    void stop();
    SdiOutputStatus status() const;

private:
    void run(SdiOutputConfig cfg);

    GstElement*        pipeline_ = nullptr;
    GstElement*        videoSrc_ = nullptr;
    GstElement*        audioSrc_ = nullptr;
    std::atomic<bool>  stopping_{false};
    std::thread        thread_;
    mutable std::mutex mutex_;
    SdiOutputStatus    status_;
};

}  // namespace testsignal
