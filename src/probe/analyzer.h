// Per-source analysis, cross-source delay and lip sync.
//
// Analyzer (one per source): finds the flash, locates the test picture inside a
// larger frame, and reads the marker's frame number.
//
//   Location. A source may show the test picture scaled inside a tile. On the
//   first flash, the pixels that jump from dark to bright between the previous
//   frame and the flash frame outline exactly where the picture sits. That
//   rectangle becomes the region for flash detection and the marker's scaled
//   position. A region given on the command line is used as-is.
//
// MuteDetector (one per source with audio): finds where the tone stops.
//
// Correlator:
//   delay     each frame number seen on a source against the same frame number
//             on the reference: arrival - reference arrival - offset. Flash
//             starts are paired the same way as a cross-check, and work when no
//             marker survives.
//   lip sync  each mute start against the nearest flash start on the same
//             source: positive means the audio is late.
#pragma once

#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "probe/marker_decode.h"
#include "probe/probe_types.h"

namespace testsignal {

struct Region {
    int x = 0, y = 0, w = 0, h = 0;
    bool valid() const { return w > 0 && h > 0; }
};

struct Observation {
    std::int64_t  arrivalNs = 0;
    std::int64_t  frameNumber = -1;    // -1: no marker read
    std::uint64_t runTag = 0;
    bool          flash = false;
    bool          flashStart = false;
    // When the flash began. On an interlaced frame whose earlier field is dark
    // and later field bright, that is half a frame after the frame arrived.
    std::int64_t  flashNs = 0;
    double        meanLuma = 0.0;
    double        framePeriodNs = 0.0;
};

class Analyzer {
public:
    explicit Analyzer(Region fixedRegion = {});

    // The sender's raster, which the marker's scaled position is derived from.
    void setSourceRaster(int width, int height);

    Observation analyse(const LumaFrame& frame);

    Region region() const { return region_; }
    bool   regionFromFlash() const { return calibrated_; }

private:
    Region                    region_;
    bool                      fixed_ = false;
    bool                      calibrated_ = false;
    int                       sourceW_ = 1920, sourceH_ = 1080;
    double                    baseline_ = -1.0;
    bool                      prevFlash_ = false;
    std::vector<std::uint8_t> prev_;
    int                       prevW_ = 0, prevH_ = 0;
};

class MuteDetector {
public:
    // Appends the start time of every tone-to-silence transition in `block`.
    // Windows are 1 ms; the start is then refined to the first silent sample.
    void feed(const AudioBlock& block, std::vector<std::int64_t>& muteStarts);

    bool   hearingTone() const { return toneSeen_; }
    double toneDbfs() const;   // RMS of the last loud window, as dBFS of a sine peak

private:
    std::vector<float> pending_;
    double             pendingStartNs_ = 0.0;
    int                rate_ = 48000;
    bool               loud_ = false;
    bool               toneSeen_ = false;
    double             lastLoudRms_ = 0.0;
    std::vector<float> lastWindow_;
};

struct DelayStats {
    std::uint64_t n = 0;
    double        sumMs = 0.0, minMs = 0.0, maxMs = 0.0;
    void add(double ms);
    double mean() const { return n ? sumMs / double(n) : 0.0; }
};

class Correlator {
public:
    // `reference` may be empty when only lip sync is being measured.
    Correlator(std::string reference, std::FILE* csv);

    void observe(const std::string& source, const Observation& o, double offsetFrames);
    void observeMute(const std::string& source, std::int64_t muteNs);
    void noteAudio(const std::string& source, double toneDbfs);

    // Interval report since the previous call, one line per source.
    std::vector<std::string> report();

private:
    struct SourceState {
        std::int64_t  lastFrame = -1;
        DelayStats    interval, total;
        std::uint64_t framesNoMarker = 0;
        std::uint64_t framesUnmatched = 0;
        std::string   lastFlashDelay;
        double        offsetFrames = 0.0;
        double        periodNs = 0.0;
        std::deque<std::int64_t> flashes;   // this source's flash starts
        std::deque<std::int64_t> mutes;     // unpaired mute starts
        DelayStats    lipInterval, lipTotal;
        bool          audio = false;
        double        toneDbfs = 0.0;
    };

    void pairMutes(SourceState& s, std::int64_t now);

    std::string                                  reference_;
    std::FILE*                                   csv_;
    std::mutex                                   mutex_;
    std::unordered_map<std::uint32_t, std::int64_t> refArrival_;
    std::deque<std::uint32_t>                    refOrder_;
    std::uint64_t                                refRunTag_ = 0;
    std::map<std::string, SourceState>           sources_;
};

}  // namespace testsignal
