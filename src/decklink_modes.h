// DeckLink output modes for the sender's formats, as GStreamer's decklink
// plugin names them, with the caps the card expects for each.
//
// Header-only and free of GStreamer so the table can be tested anywhere.
#pragma once

#include "pcapreplay/sdi_format.h"

namespace testsignal {

struct DeckLinkMode {
    const char* nick;           // decklinkvideosink mode
    const char* pixelAspect;    // pixel-aspect-ratio
    const char* colorimetry;
    const char* fieldOrder;     // nullptr for progressive
};

inline const DeckLinkMode* deckLinkModeFor(pcapreplay::SdiFormat format) {
    using F = pcapreplay::SdiFormat;
    static constexpr DeckLinkMode modes[] = {
        {"1080i50", "1/1", "bt709", "top-field-first"},
        {"1080i5994", "1/1", "bt709", "top-field-first"},
        {"1080i60", "1/1", "bt709", "top-field-first"},
        {"1080p25", "1/1", "bt709", nullptr},
        {"1080p2997", "1/1", "bt709", nullptr},
        {"1080p30", "1/1", "bt709", nullptr},
        {"1080p50", "1/1", "bt709", nullptr},
        {"1080p5994", "1/1", "bt709", nullptr},
        {"1080p60", "1/1", "bt709", nullptr},
        {"720p50", "1/1", "bt709", nullptr},
        {"720p5994", "1/1", "bt709", nullptr},
        {"720p60", "1/1", "bt709", nullptr},
        {"pal", "12/11", "bt601", "top-field-first"},
        {"ntsc", "10/11", "bt601", "bottom-field-first"},
    };
    switch (format) {
        case F::HD1080i25:   return &modes[0];
        case F::HD1080i2997: return &modes[1];
        case F::HD1080i30:   return &modes[2];
        case F::HD1080p25:   return &modes[3];
        case F::HD1080p2997: return &modes[4];
        case F::HD1080p30:   return &modes[5];
        case F::HD1080p50:   return &modes[6];
        case F::HD1080p5994: return &modes[7];
        case F::HD1080p60:   return &modes[8];
        case F::HD720p50:    return &modes[9];
        case F::HD720p5994:  return &modes[10];
        case F::HD720p60:    return &modes[11];
        case F::SD625i25:    return &modes[12];
        case F::SD525i2997:  return &modes[13];
        default:             return nullptr;   // e.g. 1080PsF25: no DeckLink mode
    }
}

// Embedded audio channels for the DeckLink sink, which takes 2, 8 or 16.
inline int deckLinkAudioChannels(int groups) { return groups <= 2 ? 8 : 16; }

}  // namespace testsignal
