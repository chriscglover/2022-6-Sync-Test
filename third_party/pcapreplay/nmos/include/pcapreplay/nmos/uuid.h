// Stable identifiers and NMOS version stamps.
//
// NMOS resource IDs have to survive a restart. If they did not, every restart
// would look to the registry like a brand new node and every route a controller
// had made to this sender would be pointing at an ID that no longer exists. So
// they are derived deterministically -- UUID v5, which is a SHA-1 over a
// namespace and a name -- from the machine name, the app and the resource role.
// Same machine, same app, same IDs, for ever.
#pragma once

#include <cstdint>
#include <string>

namespace pcapreplay::nmos {

// RFC 4122 UUID v5. `name` is hashed under `ns`, itself a UUID string.
std::string uuidV5(const std::string& ns, const std::string& name);

// A fixed namespace for this application's resources.
extern const char* const kPcapReplayNamespace;

// Convenience: the v5 UUID of "<seed>/<role>" in the app namespace.
std::string resourceId(const std::string& seed, const std::string& role);

// NMOS resource version: "<seconds>:<nanoseconds>" on the TAI timescale.
//
// TAI, not UTC. The registry orders resource updates by this string, so a
// version that goes backwards makes an update look stale and it is silently
// dropped. The offset is the current TAI-UTC difference, which is a leap-second
// table in principle; in practice it has been 37 s since 2017 and a second of
// absolute error is harmless here because only the ordering matters.
std::string taiVersion();

// Random-ish but valid UUID v4, for things that need not be stable.
std::string uuidV4();

}  // namespace pcapreplay::nmos
