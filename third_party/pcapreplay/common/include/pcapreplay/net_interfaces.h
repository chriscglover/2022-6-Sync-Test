// Network interface enumeration, for the GUI's per-path NIC selectors and the
// CLI's --iface.
//
// Multicast selects its outgoing interface *by address* on both platforms, and
// getting that wrong on a multi-homed box is the classic silent failure:
// traffic leaves via the default route and nothing appears on the link you were
// watching. The reference Windows machine has a 10G Mellanox and a Hyper-V
// vSwitch; a Linux workstation with Docker on it has docker0 and a bridge per
// compose network, all up and all multicast-capable. The selector is not
// optional on either -- see docs/04-pacing-and-network.md.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pcapreplay {

struct NetInterface {
    std::string   name;          // friendly name, e.g. "10G Ethernet"
    std::string   description;   // adapter description
    std::string   ipv4;          // dotted quad -- what IP_MULTICAST_IF wants
    std::uint32_t ipv4Be = 0;    // same address in network byte order
    std::uint32_t index  = 0;    // interface index
    std::uint64_t speedBps = 0;  // link speed, 0 if unknown
    // Hyphenated lower case, e.g. "02-00-5e-00-00-01". NMOS publishes it as the
    // node interface's chassis_id and port_id, and the SDP as ts-refclk:localmac.
    std::string   mac;
    bool          up = false;
    bool          loopback = false;
    bool          multicastCapable = false;

    // Hyper-V vSwitch, TAP adapters and the like. These frequently report the
    // same link speed as the physical NIC beside them, so without this the
    // vSwitch can sort first and become the default selection -- which is the
    // classic "multicast silently left via the wrong interface" failure.
    bool          virtualAdapter = false;

    // "10G Ethernet - 192.168.1.50 (10 Gb/s)" for the combo box.
    std::string displayName() const;
};

// Enumerate IPv4 interfaces. Returns operational, multicast-capable adapters
// first; pass includeDown to get everything.
std::vector<NetInterface> enumerateInterfaces(bool includeDown = false);

// Look one up by its IPv4 address, for restoring a saved selection.
bool findInterfaceByIp(const std::string& ipv4, NetInterface& out);

// Look one up by its interface name -- "ens18", "eth0". The CLI takes either
// form for --iface, because on Linux the name is what everything else on the
// machine calls it and making someone read an address out of `ip addr` first
// is needless friction.
bool findInterfaceByName(const std::string& name, NetInterface& out);

// Order for presentation: up before down, physical before virtual, fastest
// first, loopback last. Shared by both platforms' enumerators.
void sortInterfaces(std::vector<NetInterface>& out);

// Basic validation for the GUI's address fields.
bool isValidMulticastGroup(const std::string& ipv4);
bool parseIpv4(const std::string& s, std::uint32_t& beOut);

}  // namespace pcapreplay
