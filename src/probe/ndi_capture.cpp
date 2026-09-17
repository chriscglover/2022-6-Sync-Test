#include "probe/ndi_capture.h"

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

// gst_parse_launch quoting: NDI names carry spaces and brackets.
std::string quoted(const std::string& value) {
    std::string out = "\"";
    for (char c : value) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

}  // namespace

NdiCapture::~NdiCapture() { stop(); }

bool NdiCapture::start(const NdiSourceConfig& cfg, FrameSink frames, AudioSink audio, std::string& error) {
    std::call_once(g_gstInit, [] { gst_init(nullptr, nullptr); });
    stop();
    frameSink_ = std::move(frames);
    audioSink_ = std::move(audio);

    // Luma is all the analysis reads, so convert straight to GRAY8. Audio is
    // left at its own channel count; only the first channel is analysed.
    const std::string description =
        "ndisrc ndi-name=" + quoted(cfg.name) + " timestamp-mode=" + cfg.timestampMode +
        " ! ndisrcdemux name=demux"
        " demux.video ! queue max-size-buffers=8 ! videoconvert ! video/x-raw,format=GRAY8"
        " ! appsink name=vsink sync=false max-buffers=8 drop=false enable-last-sample=false"
        " demux.audio ! queue max-size-buffers=64 ! audioconvert"
        " ! audio/x-raw,format=F32LE,layout=interleaved,rate=48000"
        " ! appsink name=asink sync=false max-buffers=64 drop=false enable-last-sample=false";

    GError* gerror = nullptr;
    pipeline_ = gst_parse_launch(description.c_str(), &gerror);
    if (gerror) {
        error = std::string("GStreamer pipeline: ") + gerror->message +
                " (is the NDI plugin on GST_PLUGIN_PATH?)";
        g_error_free(gerror);
        stop();
        return false;
    }
    videoSink_ = gst_bin_get_by_name(GST_BIN(pipeline_), "vsink");
    audioSinkElement_ = gst_bin_get_by_name(GST_BIN(pipeline_), "asink");

    GstClock* clock = gst_system_clock_obtain();
    g_object_set(clock, "clock-type", GST_CLOCK_TYPE_MONOTONIC, nullptr);
    gst_pipeline_use_clock(GST_PIPELINE(pipeline_), clock);
    gst_object_unref(clock);

    if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        error = "NDI receive would not start: " + busError(pipeline_);
        stop();
        return false;
    }
    stopping_.store(false);
    videoThread_ = std::thread([this] { pullVideo(); });
    audioThread_ = std::thread([this] { pullAudio(); });
    return true;
}

void NdiCapture::stop() {
    stopping_.store(true);
    if (videoThread_.joinable()) videoThread_.join();
    if (audioThread_.joinable()) audioThread_.join();
    if (pipeline_) gst_element_set_state(pipeline_, GST_STATE_NULL);
    if (videoSink_) { gst_object_unref(videoSink_); videoSink_ = nullptr; }
    if (audioSinkElement_) { gst_object_unref(audioSinkElement_); audioSinkElement_ = nullptr; }
    if (pipeline_) { gst_object_unref(pipeline_); pipeline_ = nullptr; }
}

NdiStats NdiCapture::stats() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return stats_;
}

void NdiCapture::pullVideo() {
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
        const auto* data = static_cast<const std::uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&vf, 0));
        const int stride = GST_VIDEO_FRAME_PLANE_STRIDE(&vf, 0);
        frame.width = w;
        frame.height = h;
        frame.luma.resize(std::size_t(w) * std::size_t(h));
        for (int y = 0; y < h; ++y)
            std::memcpy(frame.luma.data() + std::size_t(y) * std::size_t(w),
                        data + std::size_t(y) * std::size_t(stride), std::size_t(w));
        const GstClockTime pts = GST_BUFFER_PTS(buffer);
        frame.arrivalNs = std::int64_t(gst_element_get_base_time(pipeline_) + pts);
        frame.fpsNum = GST_VIDEO_INFO_FPS_N(&info);
        frame.fpsDen = GST_VIDEO_INFO_FPS_D(&info);
        frame.interlaced = GST_VIDEO_INFO_IS_INTERLACED(&info);
        frame.bottomFieldFirst = false;
        frame.format = std::to_string(w) + "x" + std::to_string(h) + (frame.interlaced ? "i" : "p") +
                       std::to_string(frame.fpsDen ? (frame.fpsNum + frame.fpsDen / 2) / frame.fpsDen : 0) +
                       " NDI";
        gst_video_frame_unmap(&vf);
        gst_sample_unref(sample);
        {
            std::lock_guard<std::mutex> lk(mutex_);
            ++stats_.frames;
            stats_.format = frame.format;
        }
        if (GST_CLOCK_TIME_IS_VALID(pts)) frameSink_(frame);
    }
}

void NdiCapture::pullAudio() {
    AudioBlock block;
    while (!stopping_.load(std::memory_order_relaxed)) {
        GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(audioSinkElement_), 200 * GST_MSECOND);
        if (!sample) continue;
        GstBuffer* buffer = gst_sample_get_buffer(sample);
        GstStructure* s = gst_caps_get_structure(gst_sample_get_caps(sample), 0);
        int channels = 0;
        gst_structure_get_int(s, "channels", &channels);
        GstMapInfo map;
        const GstClockTime pts = GST_BUFFER_PTS(buffer);
        if (channels > 0 && GST_CLOCK_TIME_IS_VALID(pts) && gst_buffer_map(buffer, &map, GST_MAP_READ)) {
            const std::size_t frames = map.size / (4 * std::size_t(channels));
            block.samples.resize(frames);
            for (std::size_t i = 0; i < frames; ++i)
                std::memcpy(&block.samples[i], map.data + i * 4 * std::size_t(channels), 4);
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
