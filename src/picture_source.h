// The moving background: GStreamer's videotestsrc ball, pulled one picture per
// frame.
//
// The pipeline is not live and the sink does not sync, so GStreamer produces a
// picture exactly when one is asked for and the ball advances one step per
// frame. All timing belongs to the sender's pacer, not to a GStreamer clock.
#pragma once

#include <cstdint>
#include <string>

typedef struct _GstElement GstElement;

namespace testsignal {

class PictureSource {
public:
    PictureSource() = default;
    ~PictureSource();
    PictureSource(const PictureSource&) = delete;
    PictureSource& operator=(const PictureSource&) = delete;

    bool open(int width, int height, int frameRateNum, int frameRateDen, std::string& error);
    void close();

    // Copy the next picture into `dst`: UYVY, stride width * 2.
    bool pull(std::uint8_t* dst, std::string& error);

private:
    std::string busError() const;

    GstElement* pipeline_ = nullptr;
    GstElement* sink_ = nullptr;
    int width_ = 0;
    int height_ = 0;
};

}  // namespace testsignal
