#include "composer.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <execution>

#include "pcapreplay/bitpack.h"
#include "pcapreplay/crc.h"

namespace testsignal {

using namespace pcapreplay;

WireLine wireLine(const SdiFormatInfo& fi, int line) {
    WireLine w;
    if (fi.id == SdiFormat::SD525i2997) {
        // SMPTE 125M: F=1 on lines 1..3 and 266..525; V=1 on 1..19 and
        // 264..282; picture on 21..263 and 283..525. Field 2 carries the top
        // row, so the picture is bottom field first.
        w.f = line <= 3 || line >= 266;
        w.v = line <= 19 || (line >= 264 && line <= 282);
        if (line >= 21 && line <= 263)       w.activeRow = (line - 21) * 2 + 1;
        else if (line >= 283 && line <= 525) w.activeRow = (line - 283) * 2;
        return w;
    }
    const LineFlags lf = lineFlags(fi, line);
    w.f = lf.f;
    w.v = lf.v;
    w.activeRow = activeRowForLine(fi, line);
    return w;
}

FrameComposer::FrameComposer(const ComposerSettings& settings)
    : settings_(settings),
      fi_(formatInfo(settings.format)),
      rate_(timecodeRate(fi_.frameRateNum, fi_.frameRateDen)),
      builder_(settings.format),
      audio_(fi_, settings.audio) {
    std::string name = fi_.name;
    for (char& c : name) c = char(std::toupper(static_cast<unsigned char>(c)));
    char buf[160];
    if (settings_.flashPeriodFrames > 0)
        std::snprintf(buf, sizeof buf, "%s  %.0f HZ %.0f DBFS CH1, -%g DB/CH  FLASH+MUTE EVERY %d FRAMES",
                      name.c_str(), settings_.audio.toneHz, settings_.audio.levelDbfs,
                      settings_.audio.stepDb, settings_.flashPeriodFrames);
    else
        std::snprintf(buf, sizeof buf, "%s  %.0f HZ %.0f DBFS CH1, -%g DB/CH", name.c_str(),
                      settings_.audio.toneHz, settings_.audio.levelDbfs, settings_.audio.stepDb);
    detailText_ = buf;

    lineWords_ = fi_.totalSamples * 2;
    lines_.resize(std::size_t(fi_.totalLines));
    lineIndex_.resize(std::size_t(fi_.totalLines));
    for (int i = 0; i < fi_.totalLines; ++i) {
        lines_[std::size_t(i)] = wireLine(fi_, i + 1);
        lineIndex_[std::size_t(i)] = i;
    }
    wire_.assign(std::size_t(lineWords_) * std::size_t(fi_.totalLines), 0);
    packed_.assign(std::size_t(fi_.bytesPerFrame()), 0);
}

bool FrameComposer::isFlash(std::uint64_t frameIndex) const {
    return settings_.flashPeriodFrames > 0 &&
           frameIndex % std::uint64_t(settings_.flashPeriodFrames) <
               std::uint64_t(settings_.flashFrames);
}

std::string FrameComposer::timecodeTextFor(std::uint64_t frameIndex) const {
    return timecodeText(timecodeFromFrames(
        settings_.timecodeStartFrames + std::int64_t(frameIndex), rate_));
}

std::span<const std::uint8_t> FrameComposer::compose(UyvyImage picture,
                                                     std::uint64_t frameIndex,
                                                     std::uint64_t captureUtcMs) {
    drawPicture(picture, frameIndex, captureUtcMs);
    audio_.embed(builder_, frameIndex, isFlash(frameIndex));
    serialise(picture);
    return {packed_.data(), packed_.size()};
}

void FrameComposer::drawPicture(UyvyImage picture, std::uint64_t frameIndex,
                                std::uint64_t captureUtcMs) {
    const bool flash = isFlash(frameIndex);
    if (flash) fillFrame(picture, kWhiteY, kNeutralC, kNeutralC);

    // Drawn after the flash so the timecode stays legible on the flash frame,
    // which is the one frame most worth reading.
    const int h = picture.height;
    int y = h * 3 / 100;
    y += drawLabel(picture, y, h / 108, settings_.title) + h / 60;
    y += drawLabel(picture, y, h / 45, timecodeTextFor(frameIndex)) + h / 60;
    char frameText[32];
    std::snprintf(frameText, sizeof frameText, "FRAME %08llu",
                  static_cast<unsigned long long>(frameIndex));
    y += drawLabel(picture, y, h / 90, frameText) + h / 90;
    drawLabel(picture, y, h / 180, detailText_);

    drawMarker(picture, settings_.runTag, std::uint32_t(frameIndex), captureUtcMs);
}

void FrameComposer::serialise(UyvyImage picture) {
    const int lw = lineWords_;
    const int aw = fi_.activeWidth * 2;
    const int post = postActiveWords(fi_);   // EAV (+LN +CRC on HD)
    const int sav = savWords(fi_);
    const int savAt = lw - aw - sav;         // SAV offset within a wire line
    const std::span<std::uint16_t> built = builder_.words();

    // Pass 1: every line independently -- timing references, line number and
    // HANC from the builder, then the picture.
    std::for_each(std::execution::par_unseq, lineIndex_.begin(), lineIndex_.end(), [&](int i) {
        const WireLine& wl = lines_[std::size_t(i)];
        const std::uint16_t* src = built.data() + std::size_t(i) * std::size_t(lw);
        std::uint16_t* dst = wire_.data() + std::size_t(i) * std::size_t(lw);

        // EAV, LN, CRC placeholder and HANC, exactly as the builder has them...
        std::copy(src + aw, src + lw - sav, dst);
        // ...with both timing references flagged for this line.
        const std::uint16_t eav = timingXyz(wl.f, wl.v, true);
        const std::uint16_t savXyz = timingXyz(wl.f, wl.v, false);
        if (fi_.isHd) {
            dst[6] = dst[7] = eav;
            const std::uint16_t s[8] = {0x3FF, 0x3FF, 0x000, 0x000, 0x000, 0x000, savXyz, savXyz};
            std::copy(s, s + 8, dst + savAt);
        } else {
            dst[3] = eav;
            const std::uint16_t s[4] = {0x3FF, 0x000, 0x000, savXyz};
            std::copy(s, s + 4, dst + savAt);
        }

        std::uint16_t* active = dst + (lw - aw);
        if (wl.activeRow >= 0) {
            uyvy8ToWords(picture.data + std::size_t(wl.activeRow) * std::size_t(picture.stride),
                         std::size_t(fi_.activeWidth), active);
        } else {
            for (int k = 0; k < aw; ++k) active[k] = (k & 1) ? kBlankLuma : kBlankChroma;
        }
    });

    // Pass 2 (HD): the CRC in each line's header covers the active picture that
    // precedes its EAV -- the tail of the previous wire line, wrapping to the
    // last line for line 1 -- plus its EAV and LN words, per stream.
    if (fi_.isHd) {
        const std::size_t cover = std::size_t(fi_.activeWidth) + 4 + 2;
        std::for_each(std::execution::par_unseq, lineIndex_.begin(), lineIndex_.end(), [&](int i) {
            std::uint16_t* dst = wire_.data() + std::size_t(i) * std::size_t(lw);
            const int prev = (i == 0) ? fi_.totalLines - 1 : i - 1;
            const std::uint16_t* prevActive =
                wire_.data() + std::size_t(prev) * std::size_t(lw) + std::size_t(lw - aw);
            std::uint32_t crc[2];
            if (i != 0) {
                crc[0] = crc18Strided(prevActive, cover, 2);
                crc[1] = crc18Strided(prevActive + 1, cover, 2);
            } else {
                // Not contiguous across the frame boundary; continue the register.
                const std::size_t head = std::size_t(fi_.activeWidth);
                for (int s = 0; s < 2; ++s) {
                    const std::uint32_t partial = crc18Strided(prevActive + s, head, 2);
                    crc[s] = crc18Strided(dst + s, 6, 2, partial);
                }
            }
            const Crc18Words c = crc18ToWords(crc[0]);
            const Crc18Words y = crc18ToWords(crc[1]);
            dst[12] = c.crc0;
            dst[13] = y.crc0;
            dst[14] = c.crc1;
            dst[15] = y.crc1;
        });
    }
    (void)post;

    pack10(wire_.data(), wire_.size(), packed_.data());
}

}  // namespace testsignal
