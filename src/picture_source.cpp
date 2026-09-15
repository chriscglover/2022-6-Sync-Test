#include "picture_source.h"

#include <cstring>
#include <mutex>

#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video.h>

namespace testsignal {

namespace {

std::once_flag g_gstInit;

}  // namespace

PictureSource::~PictureSource() { close(); }

bool PictureSource::open(int width, int height, int frameRateNum, int frameRateDen,
                         std::string& error) {
    std::call_once(g_gstInit, [] { gst_init(nullptr, nullptr); });
    close();

    const std::string description =
        "videotestsrc is-live=false pattern=ball animation-mode=frames"
        " ! video/x-raw,format=UYVY,width=" + std::to_string(width) +
        ",height=" + std::to_string(height) +
        ",framerate=" + std::to_string(frameRateNum) + "/" + std::to_string(frameRateDen) +
        ",pixel-aspect-ratio=1/1,interlace-mode=progressive"
        " ! appsink name=sink sync=false max-buffers=2 drop=false enable-last-sample=false";

    GError* gerror = nullptr;
    pipeline_ = gst_parse_launch(description.c_str(), &gerror);
    if (gerror) {
        error = std::string("GStreamer pipeline: ") + gerror->message;
        g_error_free(gerror);
        close();
        return false;
    }
    if (!pipeline_) {
        error = "GStreamer pipeline could not be created";
        return false;
    }
    sink_ = gst_bin_get_by_name(GST_BIN(pipeline_), "sink");
    if (!sink_) {
        error = "GStreamer pipeline has no appsink";
        close();
        return false;
    }
    if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        error = "GStreamer pipeline would not start: " + busError();
        close();
        return false;
    }
    width_ = width;
    height_ = height;
    return true;
}

void PictureSource::close() {
    if (pipeline_) gst_element_set_state(pipeline_, GST_STATE_NULL);
    if (sink_) {
        gst_object_unref(sink_);
        sink_ = nullptr;
    }
    if (pipeline_) {
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
    }
}

std::string PictureSource::busError() const {
    if (!pipeline_) return "no pipeline";
    GstBus* bus = gst_element_get_bus(pipeline_);
    std::string text = "no error reported";
    if (GstMessage* msg = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR)) {
        GError* err = nullptr;
        gchar* debug = nullptr;
        gst_message_parse_error(msg, &err, &debug);
        text = err ? err->message : "unknown error";
        if (err) g_error_free(err);
        g_free(debug);
        gst_message_unref(msg);
    }
    gst_object_unref(bus);
    return text;
}

bool PictureSource::pull(std::uint8_t* dst, std::string& error) {
    GstSample* sample = gst_app_sink_pull_sample(GST_APP_SINK(sink_));
    if (!sample) {
        error = "picture source stopped: " + busError();
        return false;
    }
    GstVideoInfo info;
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    GstVideoFrame frame;
    bool ok = gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) &&
              GST_VIDEO_INFO_WIDTH(&info) == width_ &&
              GST_VIDEO_INFO_HEIGHT(&info) == height_ &&
              gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ);
    if (!ok) {
        error = "picture source delivered an unexpected picture";
        gst_sample_unref(sample);
        return false;
    }
    const auto* src = static_cast<const std::uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
    const int stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
    const std::size_t rowBytes = std::size_t(width_) * 2;
    for (int y = 0; y < height_; ++y)
        std::memcpy(dst + std::size_t(y) * rowBytes, src + std::size_t(y) * std::size_t(stride),
                    rowBytes);
    gst_video_frame_unmap(&frame);
    gst_sample_unref(sample);
    return true;
}

}  // namespace testsignal
