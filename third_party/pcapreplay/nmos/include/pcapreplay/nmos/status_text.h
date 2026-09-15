// The NMOS half of the status panel.
//
// Separate from common/status_text.h only because the two libraries are
// separate: common knows nothing about NMOS and should not have to. Shared
// between the Win32 dialog, the stats endpoint and the CLI for the same reason
// as the other half -- see the header comment there.
#pragma once

#include <string>

#include "pcapreplay/nmos/nmos_node.h"

namespace pcapreplay::nmos {

// Whether this node is registered, and with which registry, are the first two
// questions anyone asks of a sender that a controller cannot see -- so they get
// a line each and are stated outright, rather than being left to be inferred
// from a state string.
//
// `eol` is "\r\n" for a Win32 edit control and "\n" for a terminal.
std::string statusText(const NmosStatus& n, bool enabled, const char* eol = "\r\n");

}  // namespace pcapreplay::nmos
