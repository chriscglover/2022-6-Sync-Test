#include "probe/sdi_capture.h"

#include <cstring>

#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video.h>

namespace testsignal {

namespace {

std::once_flag g_gstInit;

std::string busError(GstElement* pipeline) {
    if (!pipeline) return "no pipeline";
    GstBus* bus = gst_element_get_bus(pipeline);
    std::string text;
    while (GstMessage* msg = gst_bus_pop_filtered(bus, GstMessageType(GST_MESSAGE_ERROR | GST_MESSAGE_WARNING))) {
        GError* err = nullptr;
        gchar* debug = nullptr;
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) gst_message_parse_error(msg, &err, &debug);
        else gst_message_parse_warning(msg, &err, &debug);
        if (err) text = err->message;
        if (err) g_error_free(err);
        g_free(debug);
        gst_message_unref(msg);
    }
    gst_object_unref(bus);
    return text;
}

}  // namespace

SdiCapture::~SdiCapture() { stop(); }

bool SdiCapture::start(const SdiSourceConfig& cfg, FrameSink frames, AudioSink audio, std::string& error) {
    std::call_once(g_gstInit, [] { gst_init(nullptr, nullptr); });
    stop();
    frameSink_ = std::move(frames);
    audioSink_ = std::move(audio);

    // A DeckLink only accepts an automatic pixel format with automatic mode;
    // with a fixed mode ask for 8-bit so luma is one byte per pixel. Its audio
    // source needs the video source in the same pipeline.
    const std::string format = cfg.mode == "auto" ? "auto" : "8bit-yuv";
    const std::string device = std::to_string(cfg.device);
    const std::string description =
        "decklinkvideosrc device-number=" + device + " connection=sdi mode=" + cfg.mode +
        " video-format=" + format + " drop-no-signal-frames=true"
        " ! appsink name=vsink sync=false max-buffers=8 drop=false enable-last-sample=false"
        " decklinkaudiosrc device-number=" + device + " channels=2"
        " ! audio/x-raw,format=S32LE,rate=48000,channels=2"
        " ! appsink name=asink sync=false max-buffers=64 drop=false enable-last-sample=false";

    GError* gerror = nullptr;
    pipeline_ = gst_parse_launch(description.c_str(), &gerror);
    if (gerror) {
        error = std::string("GStreamer pipeline: ") + gerror->message;
        g_error_free(gerror);
        stop();
        return false;
    }
    videoSink_ = gst_bin_get_by_name(GST_BIN(pipeline_), "vsink");
    audioSinkElement_ = gst_bin_get_by_name(GST_BIN(pipeline_), "asink");

    // Pin the monotonic system clock; ST 2022-6 arrivals use CLOCK_MONOTONIC.
    GstClock* clock = gst_system_clock_obtain();
    g_object_set(clock, "clock-type", GST_CLOCK_TYPE_MONOTONIC, nullptr);
    gst_pipeline_use_clock(GST_PIPELINE(pipeline_), clock);
    gst_object_unref(clock);

    if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        error = "DeckLink capture would not start: " + busError(pipeline_);
        stop();
        return false;
    }
    stopping_.store(false);
    videoThread_ = std::thread([this] { pullVideo(); });
    audioThread_ = std::thread([this] { pullAudio(); });
    return true;
}

void SdiCapture::stop() {
    stopping_.store(true);
    if (videoThread_.joinable()) videoThread_.join();
    if (audioThread_.joinable()) audioThread_.join();
    if (pipeline_) gst_element_set_state(pipeline_, GST_STATE_NULL);
    if (videoSink_) { gst_object_unref(videoSink_); videoSink_ = nullptr; }
    if (audioSinkElement_) { gst_object_unref(audioSinkElement_); audioSinkElement_ = nullptr; }
    if (pipeline_) { gst_object_unref(pipeline_); pipeline_ = nullptr; }
}

SdiStats SdiCapture::stats() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return stats_;
}

void SdiCapture::pullVideo() {
    LumaFrame frame;
    while (!stopping_.load(std::memory_order_relaxed)) {
        GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(videoSink_), 200 * GST_MSECOND);
        if (!sample) {
            const std::string problem = busError(pipeline_);
            if (!problem.empty()) {
                std::lock_guard<std::mutex> lk(mutex_);
                stats_.lastProblem = problem;
            }
            continue;
        }
        GstVideoInfo info;
        GstBuffer* buffer = gst_sample_get_buffer(sample);
        GstVideoFrame vf;
        if (!gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) ||
            !gst_video_frame_map(&vf, &info, buffer, GST_MAP_READ)) {
            gst_sample_unref(sample);
            continue;
        }
        const int w = GST_VIDEO_INFO_WIDTH(&info);
        const int h = GST_VIDEO_INFO_HEIGHT(&info);
        const GstVideoFormat fmt = GST_VIDEO_INFO_FORMAT(&info);
        const auto* data = static_cast<const std::uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&vf, 0));
        const int stride = GST_VIDEO_FRAME_PLANE_STRIDE(&vf, 0);

        frame.width = w;
        frame.height = h;
        frame.luma.resize(std::size_t(w) * std::size_t(h));
        bool ok = true;
        if (fmt == GST_VIDEO_FORMAT_UYVY) {
            for (int y = 0; y < h; ++y) {
                const std::uint8_t* src = data + std::size_t(y) * std::size_t(stride);
                std::uint8_t* dst = frame.luma.data() + std::size_t(y) * std::size_t(w);
                for (int x = 0; x < w; ++x) dst[x] = src[2 * x + 1];
            }
        } else if (fmt == GST_VIDEO_FORMAT_v210) {
            // v210: little-endian 32-bit words of three 10-bit samples, four
            // words per six pixels: Cb Y Cr | Y Cb Y | Cr Y Cb | Y Cr Y.
            for (int y = 0; y < h; ++y) {
                const std::uint8_t* src = data + std::size_t(y) * std::size_t(stride);
                std::uint8_t* dst = frame.luma.data() + std::size_t(y) * std::size_t(w);
                int x = 0;
                for (int g = 0; x < w; ++g) {
                    std::uint32_t word[4];
                    std::memcpy(word, src + std::size_t(g) * 16, 16);
                    const std::uint32_t ys[6] = {(word[0] >> 10) & 0x3FF, word[1] & 0x3FF, (word[1] >> 20) & 0x3FF,
                                                 (word[2] >> 10) & 0x3FF, word[3] & 0x3FF, (word[3] >> 20) & 0x3FF};
                    for (int k = 0; k < 6 && x < w; ++k, ++x) dst[x] = std::uint8_t(ys[k] >> 2);
                }
            }
        } else {
            ok = false;
        }
        const GstClockTime pts = GST_BUFFER_PTS(buffer);
        frame.arrivalNs = std::int64_t(gst_element_get_base_time(pipeline_) + pts);
        frame.fpsNum = GST_VIDEO_INFO_FPS_N(&info);
        frame.fpsDen = GST_VIDEO_INFO_FPS_D(&info);
        frame.interlaced = GST_VIDEO_INFO_IS_INTERLACED(&info);
        frame.format = std::to_string(w) + "x" + std::to_string(h) + (frame.interlaced ? "i" : "p") +
                       std::to_string(frame.fpsDen ? (frame.fpsNum + frame.fpsDen / 2) / frame.fpsDen : 0) +
                       " " + gst_video_format_to_string(fmt);
        gst_video_frame_unmap(&vf);
        gst_sample_unref(sample);

        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (ok) {
                ++stats_.frames;
                stats_.format = frame.format;
            } else {
                stats_.lastProblem = std::string("unsupported pixel format ") + gst_video_format_to_string(fmt);
            }
        }
        if (ok && GST_CLOCK_TIME_IS_VALID(pts)) frameSink_(frame);
    }
}

void SdiCapture::pullAudio() {
    AudioBlock block;
    while (!stopping_.load(std::memory_order_relaxed)) {
        GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(audioSinkElement_), 200 * GST_MSECOND);
        if (!sample) continue;
        GstBuffer* buffer = gst_sample_get_buffer(sample);
        GstMapInfo map;
        const GstClockTime pts = GST_BUFFER_PTS(buffer);
        if (GST_CLOCK_TIME_IS_VALID(pts) && gst_buffer_map(buffer, &map, GST_MAP_READ)) {
            const std::size_t frames = map.size / 8;   // S32LE, two channels
            block.samples.resize(frames);
            for (std::size_t i = 0; i < frames; ++i) {
                std::int32_t v;
                std::memcpy(&v, map.data + i * 8, 4);
                block.samples[i] = float(double(v) / 2147483648.0);
            }
            block.rate = 48000;
            block.startNs = std::int64_t(gst_element_get_base_time(pipeline_) + pts);
            gst_buffer_unmap(buffer, &map);
            {
                std::lock_guard<std::mutex> lk(mutex_);
                ++stats_.audioBlocks;
            }
            if (audioSink_) audioSink_(block);
        }
        gst_sample_unref(sample);
    }
}

}  // namespace testsignal
