// Synthesis and recovery of a full SDI raster.
//
// This is the part with no library behind it. A frame source gives us active
// picture only; ST 2022-6 carries an entire SDI signal, so the sender has to
// *manufacture* one -- timing references, line numbers, CRCs, blanking and all
// -- and the receiver has to take it apart again.
//
// Line layout, in multiplexed 10-bit words. HD (ST 292/424) carries two
// interleaved streams (Cb Y Cr Y ...) so every timing word appears twice, once
// per stream; SD (ST 259) is a single stream so they appear once.
//
//   HD:  [active 2*W] [EAV 8] [LN 4] [CRC 4] [HANC ...] [SAV 8]
//   SD:  [active 2*W] [EAV 4]                [HANC ...] [SAV 4]
//
// Sample 0 of a line is the first active sample, matching ST 274's numbering.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "pcapreplay/sdi_format.h"

namespace pcapreplay {

// 10-bit blanking levels.
inline constexpr std::uint16_t kBlankLuma   = 0x040;   // 64
inline constexpr std::uint16_t kBlankChroma = 0x200;   // 512

// Words reserved after the active picture, and for SAV, per line.
int postActiveWords(const SdiFormatInfo& fi);   // EAV (+LN +CRC on HD)
int savWords(const SdiFormatInfo& fi);

// Build the 10-bit XYZ word of a timing reference.
std::uint16_t timingXyz(bool f, bool v, bool h);

// ---------------------------------------------------------------------------
// Sender side
// ---------------------------------------------------------------------------

// Builds one SDI frame at a time into an internal 10-bit word buffer, then
// packs it to the wire byte stream.
//
// The blanking skeleton -- timing references, line numbers, blanking levels --
// is written once at construction and only the active picture and the CRCs are
// touched per frame, which keeps the per-frame cost close to the pixel
// conversion itself.
class SdiFrameBuilder {
public:
    explicit SdiFrameBuilder(SdiFormat format);

    const SdiFormatInfo& info() const { return fi_; }

    // Copy one source frame's active picture into the raster. Stride is in bytes.
    // Interlaced formats take a full progressive frame and split it into the two
    // fields; this is what a frame source hands us for an interleaved frame.
    void writeActiveUyvy8(const std::uint8_t* uyvy, int strideBytes);
    void writeActiveP216(const std::uint16_t* y, int yStrideBytes,
                         const std::uint16_t* cbcr, int cbcrStrideBytes);

    // Recompute every line CRC (HD only; a no-op on SD) and pack the frame to
    // bytes. Returns the packed wire bytes for the frame.
    std::span<const std::uint8_t> finish();

    // Direct access to the word raster, for the ancillary-data writer and tests.
    std::span<std::uint16_t> line(int lineNumber);          // 1-based
    std::span<std::uint16_t> hancSpan(int lineNumber);      // blanking after EAV
    std::span<std::uint16_t> words() { return words_; }

private:
    void buildSkeleton();
    void writeTimingWords(std::uint16_t* line, int lineNumber);

    SdiFormatInfo              fi_{};
    int                        lineWords_ = 0;
    std::vector<std::uint16_t> words_;
    std::vector<std::uint8_t>  packed_;

    // finish() is the capture thread's bottleneck: at 1080p50 the CRC alone cost
    // 12.1 ms/frame, an 82 fps ceiling, and it starved the transmit threads of
    // the CPU they needed. Both halves are embarrassingly parallel -- the CRC is
    // independent per line, and pack10 maps each 4 words to 5 bytes with no
    // carry between groups -- so both are fanned out over these index ranges.
    std::vector<int>           lineIndex_;    // 1..totalLines, for the CRC pass
    std::vector<std::size_t>   packChunk_;    // word offsets, each a multiple of 4
};

// ---------------------------------------------------------------------------
// Receiver side
// ---------------------------------------------------------------------------

// Consumes the reassembled ST 2022-6 byte stream and emits complete frames.
//
// Deliberately does not trust datagram boundaries to align with frames. ST
// 2022-6 carries the SDI signal as a continuous stream, and whether a sender
// aligns frames to datagrams is exactly the kind of thing that differs between
// implementations. So we resynchronise on the EAV pattern in the data itself
// and use the line numbers to find the top of frame, which interoperates with
// either behaviour. See docs/02-st2022-6-wire-format.md.
class SdiStreamParser {
public:
    struct Frame {
        SdiFormat                  format = SdiFormat::Unknown;
        std::vector<std::uint8_t>  uyvy;      // active picture, UYVY 8-bit
        int                        width = 0;
        int                        height = 0;
        int                        strideBytes = 0;
        bool                       interlaced = false;
        std::uint64_t              frameIndex = 0;
        // Active picture lines written into `uyvy`, so it counts up to
        // activeHeight -- not to totalLines, which includes blanking. A frame
        // with fewer than activeHeight had lines lost somewhere upstream.
        int                        linesRecovered = 0;
        int                        crcErrors = 0;
    };

    void reset(SdiFormat format);
    SdiFormat format() const { return fi_.id; }

    // Feed reassembled payload bytes. Any completed frames are appended to
    // `out`. Returns false if the parser has not achieved line lock yet.
    bool feed(std::span<const std::uint8_t> bytes, std::vector<Frame>& out);

    bool   locked()      const { return locked_; }
    int    crcErrors()   const { return crcErrors_; }
    int    lineErrors()  const { return lineErrors_; }
    std::uint64_t framesEmitted() const { return framesEmitted_; }

private:
    // Two-stage lock. The 1376-byte payload is not a multiple of the 5-byte
    // packing group, so after any loss the *byte phase* of the 10-bit stream
    // shifts as well as the line position. Stage one finds the phase that makes
    // the word stream decode to plausible timing references; stage two finds
    // the EAV within that word stream.
    bool findBytePhase();
    bool findLineLock();
    void consumeLine(std::span<const std::uint16_t> line, std::vector<Frame>& out);
    void emitFrame(std::vector<Frame>& out);

    // Words are consumed with a read cursor, never by erasing from the front of
    // pending_. Erasing the front of a vector memmoves everything behind it, so
    // consuming a frame line by line that way costs O(n^2) -- about 33 MB of
    // memmove per drain at 1080i25, which throttled real output to well under
    // 1 fps. The buffer is compacted once per feed() instead.
    std::size_t          avail() const { return pending_.size() - pendingPos_; }
    const std::uint16_t* head()  const { return pending_.data() + pendingPos_; }
    void compactPending();

    SdiFormatInfo              fi_{};
    int                        lineWords_ = 0;
    std::vector<std::uint16_t> pending_;      // unpacked words awaiting lock/parse
    std::size_t                pendingPos_ = 0;  // read cursor into pending_
    Frame                      current_{};
    bool                       locked_ = false;
    bool                       phaseKnown_ = false;
    bool                       haveFrame_ = false;
    int                        crcErrors_ = 0;
    int                        lineErrors_ = 0;

    // SD raster position. ST 259 carries no line number in the EAV -- that is a
    // ST 292 addition -- so on SD the position has to be counted across lines
    // and re-anchored from the F and V bits, rather than simply read off each
    // line the way HD allows.
    int                        sdEndedLine_ = 0;
    bool                       sdAnchored_  = false;
    int                        sdPrevV_     = -1;   // -1 = no line seen yet
    std::uint64_t              framesEmitted_ = 0;
    std::vector<std::uint8_t>  bytes_;        // group-aligned once phaseKnown_
};

// Decode the HD line-number words (ST 292) and the inverse.
int  lineNumberFromWords(std::uint16_t ln0, std::uint16_t ln1);
void lineNumberToWords(int line, std::uint16_t& ln0, std::uint16_t& ln1);

}  // namespace pcapreplay
