// Bits of DNS-SD that are the same whichever API is underneath.
//
// The Windows implementation gets its records already parsed out of
// DnsServiceResolve; the POSIX one parses them off the wire itself. Either way
// what comes out is an instance name, a host, a port and a TXT map, and turning
// those four into a filled-in MdnsService is the same work -- so it is done once
// here rather than twice, slightly differently, in two files that will drift.
//
// Internal to nmos/src; not part of the public interface.
#pragma once

#include <string>
#include <vector>

#include "pcapreplay/nmos/mdns.h"

namespace pcapreplay::nmos::detail {

// "v1.0,v1.1,v1.2" -> {"v1.0","v1.1","v1.2"}. Spaces are dropped: the DNS-SD
// binding does not allow them but real kit emits them anyway.
std::vector<std::string> splitCsv(const std::string& s);

// Fill priority / apiProto / apiAuth / apiVersions from the TXT map already in
// `s`, per the IS-04 DNS-SD binding.
void applyTxtRecords(MdnsService& s);

// Fill displayName and serviceType from `s.instance`, which arrives as the
// fully-qualified "label._nmos-register._tcp.local".
//
// Which of the two registration types a registry answered on is worth keeping:
// it is the quickest way to tell an old registry from a v1.3 one.
void deriveNames(MdnsService& s);

// A bare DNS-SD type with ".local" appended if it has not got it already, which
// is the form both APIs want to be handed.
std::string qualify(const std::string& serviceType);

// Everything up to the first dot -- the DNS-SD instance label. Instance names
// may in principle contain an escaped dot; nothing in NMOS emits one, and this
// tool's own instance label is "<name> <host>:<port>", so the simple reading is
// the right one.
std::string firstLabel(const std::string& name);

// "nothing" for an empty list, so a diagnostic reads as a sentence.
std::string joinOr(const std::vector<std::string>& v, const char* sep);

}  // namespace pcapreplay::nmos::detail
