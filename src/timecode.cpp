#include "timecode.h"

#include <cmath>
#include <cstdio>

namespace testsignal {

namespace {

// Frame numbers dropped at the start of each minute that is not a tenth minute.
int dropPerMinute(TimecodeRate r) { return r.dropFrame ? (r.fps == 60 ? 4 : 2) : 0; }

}  // namespace

TimecodeRate timecodeRate(int frameRateNum, int frameRateDen) {
    TimecodeRate r;
    if (frameRateDen == 1001) {
        r.fps = int(std::lround(double(frameRateNum) / 1000.0));
        r.dropFrame = (r.fps == 30 || r.fps == 60);
    } else {
        r.fps = frameRateDen > 0 ? int(std::lround(double(frameRateNum) / frameRateDen)) : 25;
    }
    if (r.fps <= 0) r.fps = 25;
    return r;
}

std::int64_t framesPerDay(TimecodeRate r) {
    const std::int64_t d = dropPerMinute(r);
    const std::int64_t per10Min = std::int64_t(r.fps) * 600 - d * 9;
    return per10Min * 6 * 24;
}

Timecode timecodeFromFrames(std::int64_t frames, TimecodeRate r) {
    const std::int64_t day = framesPerDay(r);
    frames %= day;
    if (frames < 0) frames += day;

    const std::int64_t d = dropPerMinute(r);
    if (d) {
        // Re-insert the skipped labels so the count divides like a non-drop one.
        const std::int64_t per10Min = std::int64_t(r.fps) * 600 - d * 9;
        const std::int64_t perMin   = std::int64_t(r.fps) * 60 - d;
        const std::int64_t tens = frames / per10Min;
        const std::int64_t rem  = frames % per10Min;
        frames += d * 9 * tens;
        if (rem > d) frames += d * ((rem - d) / perMin);
    }

    Timecode tc;
    tc.dropFrame = r.dropFrame;
    tc.f = int(frames % r.fps); frames /= r.fps;
    tc.s = int(frames % 60);    frames /= 60;
    tc.m = int(frames % 60);    frames /= 60;
    tc.h = int(frames % 24);
    return tc;
}

std::int64_t framesFromTimecode(const Timecode& tc, TimecodeRate r) {
    const std::int64_t minutes = std::int64_t(tc.h) * 60 + tc.m;
    std::int64_t frames = (minutes * 60 + tc.s) * r.fps + tc.f;
    const std::int64_t d = dropPerMinute(r);
    if (d) frames -= d * (minutes - minutes / 10);
    return frames;
}

std::string timecodeText(const Timecode& tc) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02d:%02d:%02d%c%02d",
                  tc.h, tc.m, tc.s, tc.dropFrame ? ';' : ':', tc.f);
    return buf;
}

bool parseTimecode(const std::string& text, Timecode& out) {
    int h = 0, m = 0, s = 0, f = 0;
    char sep = 0;
    if (std::sscanf(text.c_str(), "%d:%d:%d%c%d", &h, &m, &s, &sep, &f) != 5) return false;
    if ((sep != ':' && sep != ';') || h < 0 || h > 23 || m < 0 || m > 59 ||
        s < 0 || s > 59 || f < 0 || f > 59)
        return false;
    out.h = h; out.m = m; out.s = s; out.f = f;
    return true;
}

}  // namespace testsignal
