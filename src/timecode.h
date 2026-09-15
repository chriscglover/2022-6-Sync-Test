// SMPTE ST 12 timecode arithmetic for the burnt-in display.
//
// The picture carries a frame count, so the timecode is derived from it rather
// than from a clock: frame N always reads the same timecode, which is what lets
// the same frame be recognised on the way in and on the way out.
#pragma once

#include <cstdint>
#include <string>

namespace testsignal {

// Nominal integer counting rate and whether drop-frame counting applies.
// 30000/1001 counts 30 with drop-frame, 60000/1001 counts 60 with drop-frame,
// 25 and 50 count as themselves.
struct TimecodeRate {
    int  fps = 25;
    bool dropFrame = false;
};

TimecodeRate timecodeRate(int frameRateNum, int frameRateDen);

struct Timecode {
    int  h = 0, m = 0, s = 0, f = 0;
    bool dropFrame = false;
};

std::int64_t framesPerDay(TimecodeRate r);

// Frames since 00:00:00:00, wrapped to one day.
Timecode     timecodeFromFrames(std::int64_t frames, TimecodeRate r);
std::int64_t framesFromTimecode(const Timecode& tc, TimecodeRate r);

// "HH:MM:SS:FF", or "HH:MM:SS;FF" when drop-frame.
std::string timecodeText(const Timecode& tc);

// Accepts HH:MM:SS:FF or HH:MM:SS;FF.
bool parseTimecode(const std::string& text, Timecode& out);

}  // namespace testsignal
