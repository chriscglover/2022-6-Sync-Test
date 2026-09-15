#include "sdi_output.h"

#include <chrono>
#include <cstring>
#include <ctime>
#include <vector>

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#include "decklink_modes.h"
#include "picture_source.h"
#include "timecode.h"

namespace testsignal {

using namespace pcapreplay;

namespace {

std::once_flag g_gstInit;

std::string busError(GstElement* pipeline) {
    if (!pipeline) return {};
    GstBus* bus = gst_element_get_bus(pipeline);
    std::string text;
    if (GstMessage* msg = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR)) {
        GError* err = nullptr;
        gchar* debug = nullptr;
        gst_message_parse_error(msg, &err, &debug);
        text = err ? err->message : "unknown GStreamer error";
        if (err) g_error_free(err);
        g_free(debug);
        gst_message_unref(msg);
    }
    gst_object_unref(bus);
    return text;
}

std::int64_t timeOfDayFrames(std::chrono::system_clock::time_point t, TimecodeRate rate) {
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

SdiOutput::~SdiOutput() { stop(); }

bool SdiOutput::start(const SdiOutputConfig& cfg, std::string& error) {
    stop();
    const SdiFormatInfo& fi = formatInfo(cfg.composer.format);
    const DeckLinkMode* mode = deckLinkModeFor(cfg.composer.format);
    if (!mode) {
        error = std::string("a DeckLink has no output mode for ") + fi.name;
        return false;
    }
    std::call_once(g_gstInit, [] { gst_init(nullptr, nullptr); });

    const int channels = deckLinkAudioChannels(cfg.composer.audio.groups);
    const std::size_t frameBytes = std::size_t(fi.activeWidth) * 2 * std::size_t(fi.activeHeight);
    const std::string videoCaps =
        "video/x-raw,format=UYVY,width=" + std::to_string(fi.activeWidth) +
        ",height=" + std::to_string(fi.activeHeight) +
        ",framerate=" + std::to_string(fi.frameRateNum) + "/" + std::to_string(fi.frameRateDen) +
        ",pixel-aspect-ratio=" + mode->pixelAspect + ",colorimetry=" + mode->colorimetry +
        (mode->fieldOrder ? std::string(",interlace-mode=interleaved,field-order=") + mode->fieldOrder
                          : std::string(",interlace-mode=progressive"));
    // More than two channels need an explicit mask; 0 means unpositioned, as
    // the DeckLink plugin itself uses.
    const std::string audioCaps =
        "audio/x-raw,format=S32LE,layout=interleaved,rate=48000,channels=" + std::to_string(channels) +
        ",channel-mask=(bitmask)0x0";
    const std::string device = std::to_string(cfg.device);
    const std::string description =
        "appsrc name=vsrc is-live=true format=time block=true max-bytes=" + std::to_string(frameBytes * 3) +
        " caps=\"" + videoCaps + "\" ! videoconvert ! decklinkvideosink device-number=" + device +
        " mode=" + mode->nick +
        " appsrc name=asrc is-live=true format=time block=true max-bytes=" + std::to_string(48000 / 5 * 4 * channels) +
        " caps=\"" + audioCaps + "\" ! decklinkaudiosink device-number=" + device;

    GError* gerror = nullptr;
    pipeline_ = gst_parse_launch(description.c_str(), &gerror);
    if (gerror) {
        error = std::string("GStreamer pipeline: ") + gerror->message +
                " (is the decklink plugin installed? see README)";
        g_error_free(gerror);
        stop();
        return false;
    }
    videoSrc_ = gst_bin_get_by_name(GST_BIN(pipeline_), "vsrc");
    audioSrc_ = gst_bin_get_by_name(GST_BIN(pipeline_), "asrc");
    if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        error = "DeckLink output would not start: " + busError(pipeline_);
        stop();
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(mutex_);
        status_ = SdiOutputStatus{};
        status_.running = true;
        status_.mode = mode->nick;
        status_.audioChannels = channels;
    }
    stopping_.store(false);
    thread_ = std::thread([this, cfg] { run(cfg); });
    return true;
}

void SdiOutput::stop() {
    stopping_.store(true);
    // Leaving PLAYING flushes the app sources, which releases a blocked push.
    if (pipeline_) gst_element_set_state(pipeline_, GST_STATE_NULL);
    if (thread_.joinable()) thread_.join();
    if (videoSrc_) { gst_object_unref(videoSrc_); videoSrc_ = nullptr; }
    if (audioSrc_) { gst_object_unref(audioSrc_); audioSrc_ = nullptr; }
    if (pipeline_) { gst_object_unref(pipeline_); pipeline_ = nullptr; }
    std::lock_guard<std::mutex> lk(mutex_);
    status_.running = false;
}

SdiOutputStatus SdiOutput::status() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return status_;
}

void SdiOutput::run(SdiOutputConfig cfg) {
    auto fail = [&](const std::string& message) {
        std::lock_guard<std::mutex> lk(mutex_);
        status_.error = message;
        status_.running = false;
    };

    const SdiFormatInfo& fi = formatInfo(cfg.composer.format);
    const TimecodeRate rate = timecodeRate(fi.frameRateNum, fi.frameRateDen);
    const auto t0 = std::chrono::system_clock::now();
    if (cfg.timecodeFromTimeOfDay) cfg.composer.timecodeStartFrames = timeOfDayFrames(t0, rate);
    const std::int64_t t0Ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(t0.time_since_epoch()).count();
    const double frameSeconds = double(fi.frameRateDen) / double(fi.frameRateNum);

    PictureSource source;
    std::string err;
    if (!source.open(fi.activeWidth, fi.activeHeight, fi.frameRateNum, fi.frameRateDen, err)) {
        fail(err);
        return;
    }
    FrameComposer composer(cfg.composer);
    const int channels = deckLinkAudioChannels(cfg.composer.audio.groups);
    const std::size_t frameBytes = std::size_t(fi.activeWidth) * 2 * std::size_t(fi.activeHeight);
    std::vector<std::uint8_t> picture(frameBytes);
    const UyvyImage image{picture.data(), fi.activeWidth, fi.activeHeight, fi.activeWidth * 2};

    for (std::uint64_t k = 0; !stopping_.load(std::memory_order_relaxed); ++k) {
        if (cfg.maxSeconds > 0.0 && double(k) * frameSeconds >= cfg.maxSeconds) {
            std::lock_guard<std::mutex> lk(mutex_);
            status_.completed = true;
            break;
        }
        if (!source.pull(picture.data(), err)) { fail(err); return; }
        const std::uint64_t captureMs = std::uint64_t(t0Ms) + std::uint64_t(double(k) * frameSeconds * 1000.0 + 0.5);
        composer.drawPicture(image, k, captureMs);

        GstBuffer* video = gst_buffer_new_allocate(nullptr, frameBytes, nullptr);
        gst_buffer_fill(video, 0, picture.data(), frameBytes);
        GST_BUFFER_PTS(video) = gst_util_uint64_scale(k, GST_SECOND * std::uint64_t(fi.frameRateDen),
                                                      std::uint64_t(fi.frameRateNum));
        GST_BUFFER_DURATION(video) = gst_util_uint64_scale(GST_SECOND, std::uint64_t(fi.frameRateDen),
                                                           std::uint64_t(fi.frameRateNum));

        // The samples that belong to frame k, timed from the same origin.
        const std::uint64_t first = AudioEmbedder::firstSampleOfFrame(fi, k);
        const std::uint64_t count = AudioEmbedder::firstSampleOfFrame(fi, k + 1) - first;
        const bool mute = composer.isFlash(k);
        GstBuffer* audio = gst_buffer_new_allocate(nullptr, count * 4 * std::uint64_t(channels), nullptr);
        GstMapInfo map;
        gst_buffer_map(audio, &map, GST_MAP_WRITE);
        for (std::uint64_t i = 0; i < count; ++i) {
            for (int c = 0; c < channels; ++c) {
                const std::int32_t value =
                    mute ? 0 : std::int32_t(std::uint32_t(composer.audio().toneSample(first + i, c)) << 8);
                std::memcpy(map.data + (i * std::uint64_t(channels) + std::uint64_t(c)) * 4, &value, 4);
            }
        }
        gst_buffer_unmap(audio, &map);
        GST_BUFFER_PTS(audio) = gst_util_uint64_scale(first, GST_SECOND, 48000);
        GST_BUFFER_DURATION(audio) = gst_util_uint64_scale(count, GST_SECOND, 48000);

        if (gst_app_src_push_buffer(GST_APP_SRC(videoSrc_), video) != GST_FLOW_OK ||
            gst_app_src_push_buffer(GST_APP_SRC(audioSrc_), audio) != GST_FLOW_OK) {
            if (!stopping_.load()) fail("DeckLink output stopped: " + busError(pipeline_));
            return;
        }
        const std::string problem = busError(pipeline_);
        std::lock_guard<std::mutex> lk(mutex_);
        status_.framesSent = k + 1;
        status_.timecode = composer.timecodeTextFor(k);
        if (!problem.empty()) {
            status_.error = problem;
            status_.running = false;
            return;
        }
    }
}

}  // namespace testsignal
