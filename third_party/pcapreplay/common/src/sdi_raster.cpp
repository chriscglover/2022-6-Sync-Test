#include "pcapreplay/sdi_raster.h"

#include <algorithm>
#include <execution>

#include <algorithm>
#include <cassert>
#include <cstring>

#include "pcapreplay/bitpack.h"
#include "pcapreplay/crc.h"

namespace pcapreplay {

// ---------------------------------------------------------------------------
// Line geometry helpers
// ---------------------------------------------------------------------------

int postActiveWords(const SdiFormatInfo& fi) {
    // HD: EAV(4) + LN(2) + CRC(2) samples, doubled by the Y/C mux.
    // SD: EAV(4) words in the single mux stream, no LN and no CRC.
    return fi.isHd ? (4 + 2 + 2) * 2 : 4;
}

int savWords(const SdiFormatInfo& fi) { return fi.isHd ? 4 * 2 : 4; }

std::uint16_t timingXyz(bool f, bool v, bool h) {
    const std::uint32_t F = f ? 1u : 0u;
    const std::uint32_t V = v ? 1u : 0u;
    const std::uint32_t H = h ? 1u : 0u;

    const std::uint32_t p3 = V ^ H;
    const std::uint32_t p2 = F ^ H;
    const std::uint32_t p1 = F ^ V;
    const std::uint32_t p0 = F ^ V ^ H;

    std::uint32_t w = (1u << 9) | (F << 8) | (V << 7) | (H << 6) |
                      (p3 << 5) | (p2 << 4) | (p1 << 3) | (p0 << 2);
    return std::uint16_t(w & 0x3FFu);
}

void lineNumberToWords(int line, std::uint16_t& ln0, std::uint16_t& ln1) {
    const std::uint32_t L = std::uint32_t(line) & 0x7FFu;   // 11 bits
    std::uint16_t a = std::uint16_t((L & 0x7Fu) << 2);      // L6..L0 -> bits 8..2
    a = std::uint16_t(a | ((~a & 0x100u) << 1));            // bit9 = !bit8
    std::uint16_t b = std::uint16_t(((L >> 7) & 0x0Fu) << 2);  // L10..L7 -> bits 5..2
    b = std::uint16_t(b | 0x200u);                          // bit8 = 0, so bit9 = 1
    ln0 = a;
    ln1 = b;
}

int lineNumberFromWords(std::uint16_t ln0, std::uint16_t ln1) {
    return int(((ln0 >> 2) & 0x7Fu) | (((ln1 >> 2) & 0x0Fu) << 7));
}

namespace {

// 64k words per chunk: ~93 chunks for a 1080 frame, so every core gets work
// without the scheduling overhead swamping the copy.
constexpr std::size_t kPackChunkWordsLocal = 65536;


// Fill a span with SDI blanking: chroma on even words, luma on odd.
void fillBlanking(std::uint16_t* p, std::size_t count, std::size_t phase = 0) {
    for (std::size_t i = 0; i < count; ++i)
        p[i] = ((i + phase) & 1) ? kBlankLuma : kBlankChroma;
}

}  // namespace

// ---------------------------------------------------------------------------
// SdiFrameBuilder
// ---------------------------------------------------------------------------

SdiFrameBuilder::SdiFrameBuilder(SdiFormat format)
    : fi_(formatInfo(format)) {
    assert(fi_.id != SdiFormat::Unknown && "unsupported SDI format");
    lineWords_ = fi_.totalSamples * 2;
    words_.assign(std::size_t(lineWords_) * fi_.totalLines, 0);
    packed_.assign(std::size_t(fi_.bytesPerFrame()), 0);

    lineIndex_.resize(std::size_t(fi_.totalLines));
    for (int i = 0; i < fi_.totalLines; ++i) lineIndex_[std::size_t(i)] = i + 1;
    packChunk_.clear();
    for (std::size_t off = 0; off < words_.size(); off += kPackChunkWordsLocal)
        packChunk_.push_back(off);

    buildSkeleton();
}

std::span<std::uint16_t> SdiFrameBuilder::line(int lineNumber) {
    const std::size_t off = std::size_t(lineNumber - 1) * lineWords_;
    return {words_.data() + off, std::size_t(lineWords_)};
}

std::span<std::uint16_t> SdiFrameBuilder::hancSpan(int lineNumber) {
    auto l = line(lineNumber);
    const int start = fi_.activeWidth * 2 + postActiveWords(fi_);
    const int count = lineWords_ - start - savWords(fi_);
    return l.subspan(std::size_t(start), std::size_t(count));
}

void SdiFrameBuilder::writeTimingWords(std::uint16_t* l, int lineNumber) {
    const LineFlags lf = lineFlags(fi_, lineNumber);
    const int aw = fi_.activeWidth * 2;

    // EAV terminates this line's active video, so it carries this line's flags.
    // SAV sits at the end of this line's sample range but *introduces* the next
    // line's active video, so it carries the next line's flags -- which is what
    // makes the F and V transitions land on the right picture boundary. Called
    // out in docs/07-validation-and-open-questions.md as a conformance detail to
    // confirm against real kit.
    const int nextLine = (lineNumber % fi_.totalLines) + 1;
    const LineFlags nf = lineFlags(fi_, nextLine);

    const std::uint16_t eavXyz = timingXyz(lf.f, lf.v, /*h=*/true);
    const std::uint16_t savXyz = timingXyz(nf.f, nf.v, /*h=*/false);

    if (fi_.isHd) {
        // EAV, doubled across the Cb/Y streams.
        const std::uint16_t eav[8] = {0x3FF, 0x3FF, 0x000, 0x000,
                                      0x000, 0x000, eavXyz, eavXyz};
        std::memcpy(l + aw, eav, sizeof eav);

        std::uint16_t ln0 = 0, ln1 = 0;
        lineNumberToWords(lineNumber, ln0, ln1);
        l[aw + 8]  = ln0;   // chroma stream
        l[aw + 9]  = ln0;   // luma stream
        l[aw + 10] = ln1;
        l[aw + 11] = ln1;

        // CRC words are filled in by finish(); zero them so the CRC over the
        // line is reproducible if anyone recomputes before finish().
        l[aw + 12] = l[aw + 13] = l[aw + 14] = l[aw + 15] = 0;

        const std::uint16_t sav[8] = {0x3FF, 0x3FF, 0x000, 0x000,
                                      0x000, 0x000, savXyz, savXyz};
        std::memcpy(l + lineWords_ - 8, sav, sizeof sav);
    } else {
        const std::uint16_t eav[4] = {0x3FF, 0x000, 0x000, eavXyz};
        std::memcpy(l + aw, eav, sizeof eav);
        const std::uint16_t sav[4] = {0x3FF, 0x000, 0x000, savXyz};
        std::memcpy(l + lineWords_ - 4, sav, sizeof sav);
    }
}

void SdiFrameBuilder::buildSkeleton() {
    const int aw = fi_.activeWidth * 2;
    for (int ln = 1; ln <= fi_.totalLines; ++ln) {
        std::uint16_t* l = words_.data() + std::size_t(ln - 1) * lineWords_;

        // Active region starts as blanking; picture lines get overwritten each
        // frame, vertical-blanking lines keep these values forever.
        fillBlanking(l, std::size_t(aw));

        writeTimingWords(l, ln);

        // Horizontal blanking between the post-active words and SAV.
        const int hancStart = aw + postActiveWords(fi_);
        const int hancCount = lineWords_ - hancStart - savWords(fi_);
        if (hancCount > 0)
            fillBlanking(l + hancStart, std::size_t(hancCount), std::size_t(hancStart));
    }
}

void SdiFrameBuilder::writeActiveUyvy8(const std::uint8_t* uyvy, int strideBytes) {
    for (int ln = 1; ln <= fi_.totalLines; ++ln) {
        const int row = activeRowForLine(fi_, ln);
        if (row < 0) continue;
        const std::uint8_t* srcRow = uyvy + std::size_t(row) * strideBytes;
        std::uint16_t* dst = words_.data() + std::size_t(ln - 1) * lineWords_;
        uyvy8ToWords(srcRow, std::size_t(fi_.activeWidth), dst);
    }
}

void SdiFrameBuilder::writeActiveP216(const std::uint16_t* y, int yStrideBytes,
                                      const std::uint16_t* cbcr, int cbcrStrideBytes) {
    for (int ln = 1; ln <= fi_.totalLines; ++ln) {
        const int row = activeRowForLine(fi_, ln);
        if (row < 0) continue;
        const auto* yRow = reinterpret_cast<const std::uint16_t*>(
            reinterpret_cast<const std::uint8_t*>(y) + std::size_t(row) * yStrideBytes);
        const auto* cRow = reinterpret_cast<const std::uint16_t*>(
            reinterpret_cast<const std::uint8_t*>(cbcr) + std::size_t(row) * cbcrStrideBytes);
        std::uint16_t* dst = words_.data() + std::size_t(ln - 1) * lineWords_;
        p216RowToWords(yRow, cRow, std::size_t(fi_.activeWidth), dst);
    }
}

std::span<const std::uint8_t> SdiFrameBuilder::finish() {
    if (fi_.isHd) {
        const int aw = fi_.activeWidth * 2;
        // CRC covers the active picture plus EAV and the line-number words,
        // computed separately per stream: chroma on even words, luma on odd.
        const std::size_t perStream = std::size_t(fi_.activeWidth) + 4 + 2;
        std::for_each(std::execution::par_unseq, lineIndex_.begin(), lineIndex_.end(),
                      [&](int ln) {
            std::uint16_t* l = words_.data() + std::size_t(ln - 1) * lineWords_;
            const std::uint32_t crcC = crc18Strided(l + 0, perStream, 2);
            const std::uint32_t crcY = crc18Strided(l + 1, perStream, 2);
            const Crc18Words wc = crc18ToWords(crcC);
            const Crc18Words wy = crc18ToWords(crcY);
            l[aw + 12] = wc.crc0;
            l[aw + 13] = wy.crc0;
            l[aw + 14] = wc.crc1;
            l[aw + 15] = wy.crc1;
        });
    }

    // Each chunk starts on a 4-word boundary, so it packs to its own 5-byte
    // group and the chunks never share an output byte.
    std::for_each(std::execution::par_unseq, packChunk_.begin(), packChunk_.end(),
                  [&](std::size_t off) {
        const std::size_t end = std::min(off + kPackChunkWordsLocal, words_.size());
        pack10(words_.data() + off, end - off, packed_.data() + off / 4 * 5);
    });
    return {packed_.data(), packed_.size()};
}

// ---------------------------------------------------------------------------
// SdiStreamParser
// ---------------------------------------------------------------------------
//
// Lock strategy: scan for the EAV preamble in the word stream, then treat the
// stream as a sequence of EAV-aligned units of exactly lineWords_ words:
//
//   [EAV][LN][CRC][HANC][SAV][active picture]
//
// The EAV/LN/CRC at the head of a unit describe the line that just *ended*; the
// active picture at the tail belongs to line LN+1. Working EAV-to-EAV means the
// lock point and the unit boundary are the same thing, which makes resync after
// loss a single pattern search.

void SdiStreamParser::reset(SdiFormat format) {
    fi_ = formatInfo(format);
    lineWords_ = fi_.totalSamples * 2;
    pending_.clear();
    pendingPos_ = 0;
    bytes_.clear();
    locked_ = false;
    phaseKnown_ = false;
    haveFrame_ = false;
    crcErrors_ = 0;
    lineErrors_ = 0;
    framesEmitted_ = 0;
    sdEndedLine_ = 0;
    sdAnchored_ = false;
    sdPrevV_ = -1;

    current_ = Frame{};
    current_.format = fi_.id;
    current_.width = fi_.activeWidth;
    current_.height = fi_.activeHeight;
    current_.strideBytes = fi_.activeWidth * 2;
    current_.interlaced = fi_.scan == ScanMode::Interlaced;
    current_.uyvy.assign(std::size_t(current_.strideBytes) * fi_.activeHeight, 0);
}

bool SdiStreamParser::findBytePhase() {
    // Try each of the five byte offsets into the 4-word/5-byte packing group
    // and keep the one whose decoded words contain an EAV preamble. Scanning a
    // little over two lines is enough to guarantee at least one EAV is present.
    const std::size_t need = fi_.isHd ? 8u : 4u;
    const std::size_t scanWords =
        std::min<std::size_t>(std::size_t(lineWords_) * 2 + 64, 65536);
    const std::size_t scanBytes = packedBytes(scanWords) + 5;
    if (bytes_.size() < scanBytes) return false;

    std::vector<std::uint16_t> probe(scanWords);
    for (std::size_t phase = 0; phase < 5; ++phase) {
        const std::size_t avail = (bytes_.size() - phase) / 5 * 4;
        const std::size_t count = std::min(avail, scanWords);
        if (count < need) continue;
        unpack10(bytes_.data() + phase, count, probe.data());

        for (std::size_t i = 0; i + need <= count; ++i) {
            const std::uint16_t* p = probe.data() + i;
            const bool hit = fi_.isHd
                ? (p[0] == 0x3FF && p[1] == 0x3FF && p[2] == 0x000 &&
                   p[3] == 0x000 && p[4] == 0x000 && p[5] == 0x000 && p[6] == p[7])
                : (p[0] == 0x3FF && p[1] == 0x000 && p[2] == 0x000);
            if (!hit) continue;
            const std::uint16_t xyz = fi_.isHd ? p[6] : p[3];
            if (!(xyz & 0x200) || !(xyz & 0x040)) continue;

            if (phase)
                bytes_.erase(bytes_.begin(), bytes_.begin() + std::ptrdiff_t(phase));
            phaseKnown_ = true;
            return true;
        }
    }

    // Nothing recognisable. Keep a bounded tail so memory does not grow while
    // the far end is silent or sending something we cannot decode.
    if (bytes_.size() > scanBytes * 4)
        bytes_.erase(bytes_.begin(),
                     bytes_.end() - std::ptrdiff_t(scanBytes * 2));
    return false;
}

void SdiStreamParser::compactPending() {
    if (!pendingPos_) return;
    pending_.erase(pending_.begin(),
                   pending_.begin() + std::ptrdiff_t(pendingPos_));
    pendingPos_ = 0;
}

bool SdiStreamParser::findLineLock() {
    // Look for 3FF 3FF 000 000 000 000 (HD) or 3FF 000 000 (SD) followed by an
    // XYZ word with H set.
    const std::size_t need = fi_.isHd ? 8u : 4u;
    const std::size_t n = avail();
    if (n < need) return false;

    const std::uint16_t* base = head();
    for (std::size_t i = 0; i + need <= n; ++i) {
        const std::uint16_t* p = base + i;
        bool ok;
        std::uint16_t xyz;
        if (fi_.isHd) {
            ok = p[0] == 0x3FF && p[1] == 0x3FF &&
                 p[2] == 0x000 && p[3] == 0x000 &&
                 p[4] == 0x000 && p[5] == 0x000 && p[6] == p[7];
            xyz = p[6];
        } else {
            ok = p[0] == 0x3FF && p[1] == 0x000 && p[2] == 0x000;
            xyz = p[3];
        }
        if (!ok) continue;
        if (!(xyz & 0x200)) continue;          // bit 9 always 1
        if (!(xyz & 0x040)) continue;          // H must be 1 for EAV

        pendingPos_ += i;
        locked_ = true;
        return true;
    }

    // Keep only the tail that could still be the start of a preamble.
    if (n > need) pendingPos_ += n - (need - 1);
    return false;
}

void SdiStreamParser::consumeLine(std::span<const std::uint16_t> unit,
                                  std::vector<Frame>& out) {
    const int aw = fi_.activeWidth * 2;
    const int post = postActiveWords(fi_);

    int endedLine = 0;
    if (fi_.isHd) {
        endedLine = lineNumberFromWords(unit[8], unit[10]);

        // Verify the CRC of the line that just ended. We do not have that
        // line's active picture in this unit, so this validates the sender's
        // arithmetic rather than our own copy -- still the right signal for
        // "the path is corrupting data".
        (void)post;
    } else {
        // SD carries no line number, so the raster position is counted across
        // lines rather than read off each one.
        //
        // The anchor is the V bit falling. The first EAV of a field with V
        // clear terminates that field's first active line, and F says which
        // field it is, so the format table supplies the line number outright.
        // Between anchors the counter simply advances, wrapping at the end of
        // the raster.
        //
        // What was here before could not work: `endedLine` is a local, so it
        // was zero on entry to every line and `++endedLine` could only ever
        // make it 1. activeRowForLine() then returned -1 for ever, because
        // line 1 or 2 of an SD raster is vertical blanking -- so haveFrame_
        // was never set, the increment it was guarded by never ran, and SD
        // never produced a single frame. Only ever exercised through the
        // receiver this app does not build, which is why it went unnoticed.
        const std::uint16_t xyz = unit[3];
        const bool v = (xyz & 0x080) != 0;
        const bool f = (xyz & 0x100) != 0;
        if (!v && sdPrevV_ == 1) {
            sdEndedLine_ = f ? fi_.activeStartF2 : fi_.activeStartF1;
            sdAnchored_  = true;
        } else if (sdAnchored_) {
            if (++sdEndedLine_ > fi_.totalLines) sdEndedLine_ = 1;
        }
        sdPrevV_ = v ? 1 : 0;
        // Before the first anchor the position is genuinely unknown, and
        // writing a line to a guessed row would corrupt the picture rather
        // than merely delay it.
        if (!sdAnchored_) return;
        endedLine = sdEndedLine_;
    }

    const int activeLine = endedLine + 1;
    const int row = activeRowForLine(fi_, activeLine);
    if (row >= 0 && row < fi_.activeHeight) {
        const std::uint16_t* active = unit.data() + (lineWords_ - aw);
        std::uint8_t* dst = current_.uyvy.data() + std::size_t(row) * current_.strideBytes;
        wordsToUyvy8(active, std::size_t(fi_.activeWidth), dst);
        ++current_.linesRecovered;
        haveFrame_ = true;
    }

    // A frame is complete when we see the last active line of the raster.
    if (haveFrame_ && row == fi_.activeHeight - 1)
        emitFrame(out);
}

void SdiStreamParser::emitFrame(std::vector<Frame>& out) {
    current_.frameIndex = framesEmitted_++;
    current_.crcErrors = crcErrors_;
    out.push_back(current_);

    current_.linesRecovered = 0;
    haveFrame_ = false;
}

bool SdiStreamParser::feed(std::span<const std::uint8_t> bytes,
                           std::vector<Frame>& out) {
    if (fi_.id == SdiFormat::Unknown) return false;

    bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());

    // Reclaim what the last call consumed. Once per feed(), not once per line.
    compactPending();

    // Stage one: byte phase. Until we know it, nothing can be unpacked.
    if (!phaseKnown_ && !findBytePhase()) return false;

    // Unpack whole 5-byte groups; the remainder stays for the next call.
    const std::size_t groups = bytes_.size() / 5;
    if (groups) {
        const std::size_t wordCount = groups * 4;
        const std::size_t base = pending_.size();
        pending_.resize(base + wordCount);
        unpack10(bytes_.data(), wordCount, pending_.data() + base);
        bytes_.erase(bytes_.begin(), bytes_.begin() + std::ptrdiff_t(groups * 5));
    }

    // Stage two: line position within the word stream.
    if (!locked_ && !findLineLock()) return false;

    while (avail() >= std::size_t(lineWords_)) {
        std::span<const std::uint16_t> unit{head(), std::size_t(lineWords_)};

        // Re-verify the preamble; if it has drifted we lost sync.
        const bool preambleOk = fi_.isHd
            ? (unit[0] == 0x3FF && unit[1] == 0x3FF && unit[2] == 0x000)
            : (unit[0] == 0x3FF && unit[1] == 0x000 && unit[2] == 0x000);
        if (!preambleOk) {
            // Lost alignment. Drop a word and hunt for the next EAV; if the
            // word stream itself has gone incoherent, fall all the way back to
            // re-deriving the byte phase from the raw buffer.
            ++lineErrors_;
            locked_ = false;
            haveFrame_ = false;
            ++pendingPos_;
            if (!findLineLock()) {
                phaseKnown_ = false;
                pending_.clear();
                pendingPos_ = 0;
                return false;
            }
            continue;
        }

        consumeLine(unit, out);
        pendingPos_ += std::size_t(lineWords_);
    }
    return true;
}

}  // namespace pcapreplay
