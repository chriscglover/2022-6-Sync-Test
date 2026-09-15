// The NMOS node: IS-04 registration and Node API, IS-05 Connection API.
//
// What this buys you: the replay appears in an NMOS controller alongside real
// broadcast kit, and can be routed to any NMOS receiver without anyone typing a
// multicast address anywhere. The controller reads the SDP from the sender's
// manifest and hands it to the receiver.
//
// Three moving parts, all of which have to be right for that to work:
//
//   IS-04  the node registers itself, its device, and a source/flow/sender
//          chain with the registry, then heartbeats to stay alive. It also
//          serves the same resources over its own Node API, which is what a
//          controller reads when there is no registry.
//
//   IS-05  the connection API. A controller activates the sender and can move
//          it to a different multicast group. Without this, most controllers
//          will list the sender but refuse to route it.
//
//   SDP    the transport file. ST 2022-6 is a mux format, so the flow carries
//          media_type video/SMPTE2022-6 and the SDP an rtpmap of the same at
//          27 MHz. A -7 pair is two m= lines grouped with a=group:DUP, per
//          RFC 7104, which is how a receiver is told the two legs are copies of
//          each other rather than two different essences.
//
// The registry is found over mDNS (_nmos-register._tcp), with a manual host and
// port override because plenty of real networks do not carry mDNS between
// subnets. Peer-to-peer mode advertises _nmos-node._tcp so a controller can find
// this node with no registry present at all.
//
// This is a hand-rolled implementation rather than sony/nmos-cpp, so that the
// app stays a single executable with no Conan, vcpkg or Bonjour SDK to install.
// The seam is NmosBackend: swapping in nmos-cpp later means another
// implementation of that interface, not a rewrite of the app.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pcapreplay::nmos {

// What the replay engine is doing, as IS-05 transport parameters. This is the
// whole contract between the NMOS layer and the rest of the app: the NMOS side
// never touches the engine directly.
struct SenderTransport {
    bool          active = false;        // actually transmitting right now
    bool          redundant = false;     // ST 2022-7: two legs

    std::string   sourceIpA, sourceIpB;  // the NIC each leg leaves by
    std::string   destIpA = "239.1.1.1", destIpB = "239.2.1.1";
    std::uint16_t destPortA = 40000, destPortB = 40000;
    std::uint16_t sourcePortA = 5004, sourcePortB = 5004;
    int           ttl = 8;
    std::uint32_t ssrc = 0x20220006u;
    int           payloadType = 98;

    std::string   formatText;            // "1080i50  1.485 Gb/s", for labels
    std::string   interfaceNameA, interfaceNameB;
    // Published as the node interface chassis_id/port_id and, because this
    // sender is not PTP locked, as the SDP's ts-refclk:localmac.
    std::string   macA, macB;

    // Exact rational rate for the source and flow grain_rate. A controller uses
    // it to decide whether this sender is compatible with a receiver.
    int           frameRateNum = 25, frameRateDen = 1;

    bool operator==(const SenderTransport& o) const;
    bool operator!=(const SenderTransport& o) const { return !(*this == o); }
};

struct NmosConfig {
    bool          enabled = false;
    std::string   label = "PCAP Replay";
    std::string   description = "ST 2022-6/-7 replay from packet capture";

    // The interface the Node API binds to and publishes. Empty binds every
    // interface, which works but publishes a less useful href.
    std::string   nodeIp;
    std::uint16_t nodePort = 3210;       // 0 takes an ephemeral port

    // Registry discovery. mDNS first; the override wins when it is filled in,
    // for networks that do not carry mDNS across subnets.
    bool          useMdns = true;
    std::string   registryHost;
    std::uint16_t registryPort = 3210;

    std::string   registrationVersion = "v1.3";   // IS-04
    std::string   connectionVersion   = "v1.1";   // IS-05

    bool          advertisePeerToPeer = true;
    int           heartbeatSeconds = 5;

    // Seeds the deterministic resource UUIDs. Empty derives one from the
    // machine name, so the IDs are stable across restarts.
    std::string   seed;
};

struct NmosStatus {
    bool        running = false;
    std::string error;
    std::string warning;

    std::string nodeApiUrl;              // what a controller should browse to

    // Where this node is registered, and how that registry was found. These are
    // the two questions asked of a node that is misbehaving, so they are
    // reported plainly rather than left to be inferred from `registryState`.
    bool          registered = false;    // the registry has accepted us
    std::string   registryUrl;           // the Registration API base in use
    std::string   registryHost;
    std::uint16_t registryPort = 0;
    std::string   registryDiscovery;     // "mDNS" | "manual override" | ""
    std::string   registryServiceType;   // the DNS-SD type it answered on
    std::string   registryState;         // human-readable state line
    double        lastHeartbeatAgo = -1.0;   // seconds; negative = never

    bool        advertising = false;
    bool        browsing = false;        // an mDNS browse is actually running
    std::vector<std::string> browsedServiceTypes;
    std::string mdnsError;               // why the browse is not working
    std::string mdnsRejection;           // found something, but could not use it

    std::uint64_t heartbeats = 0;
    std::uint64_t heartbeatFailures = 0;
    std::uint64_t requestsServed = 0;
    std::string lastRequest;

    std::string nodeId, deviceId, sourceId, flowId, senderId;
    // The BCP-002-01 group hint published on the sender. Shown so it can be
    // read off the panel rather than fetched out of the Node API.
    std::string groupHint;
    bool        masterEnable = false;
    std::string connectedReceiverId;

    // What IS-05 last asked for and what came of it, so an activation that a
    // controller believes worked can be checked against what the node did.
    std::string lastActivation;

    std::vector<std::string> discoveredRegistries;
};

// The seam an nmos-cpp backend would implement instead.
class NmosBackend {
public:
    virtual ~NmosBackend() = default;

    // Read the engine's current transport parameters.
    using QueryFn = std::function<SenderTransport()>;
    // Apply what a controller staged and activated. Return false and set `err`
    // to reject; IS-05 turns that into a 500 with the message.
    using ApplyFn = std::function<bool(const SenderTransport& want, std::string& err)>;

    virtual void setCallbacks(QueryFn query, ApplyFn apply) = 0;
    virtual bool start(const NmosConfig& cfg) = 0;
    virtual void stop() = 0;
    virtual NmosStatus status() const = 0;
    // Tell the node its sender changed, so the registry sees a new version.
    virtual void notifyChanged() = 0;
};

// The built-in, dependency-free backend.
NmosBackend* createBuiltinBackend();

}  // namespace pcapreplay::nmos
