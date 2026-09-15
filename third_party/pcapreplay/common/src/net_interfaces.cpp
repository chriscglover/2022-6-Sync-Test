#include "pcapreplay/net_interfaces.h"

#include "pcapreplay/platform.h"

#ifdef _WIN32
#include <iphlpapi.h>
#else
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <cstdio>
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <memory>

namespace pcapreplay {
namespace {

#ifdef _WIN32
std::string wideToUtf8(const wchar_t* w) {
    if (!w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(std::size_t(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}
#endif

// Hyper-V vSwitches, docker bridges, TAP adapters and the like. These
// frequently report the same link speed as the physical NIC beside them, so
// without this a bridge can sort first and become the default selection --
// which is the classic "multicast silently left via the wrong interface"
// failure. The Linux list is the one that matters here: a workstation with
// Docker installed has docker0 and a br-* per compose network, all of them up,
// all of them multicast-capable, and none of them anywhere near the wire.
bool looksVirtualByName(const std::string& name, const std::string& desc) {
    static const char* const markers[] = {
        "hyper-v", "vethernet", "virtual", "vmware", "virtualbox",
        "tap-", "tun", "loopback", "wan miniport", "bluetooth",
        "docker", "br-", "virbr", "veth", "cni", "flannel", "kube",
        "tailscale", "wg", "zt",
    };
    std::string hay = name + " " + desc;
    std::transform(hay.begin(), hay.end(), hay.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    for (const char* m : markers)
        if (hay.find(m) != std::string::npos) return true;
    return false;
}

std::string speedText(std::uint64_t bps) {
    char b[32];
    if (bps >= 1000000000ull) std::snprintf(b, sizeof b, "%.0f Gb/s", double(bps) / 1e9);
    else if (bps >= 1000000ull) std::snprintf(b, sizeof b, "%.0f Mb/s", double(bps) / 1e6);
    else return {};
    return b;
}

#ifndef _WIN32

// Link speed and MAC come out of sysfs rather than an ioctl.
//
// SIOCETHTOOL would give the speed too, but it needs CAP_NET_ADMIN on many
// kernels and this tool is meant to run unprivileged. /sys/class/net is
// world-readable and carries both, so there is nothing to elevate for.
std::string readSysfs(const std::string& iface, const char* leaf) {
    const std::string path = "/sys/class/net/" + iface + "/" + leaf;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    char buf[128] = {};
    const std::size_t n = std::fread(buf, 1, sizeof buf - 1, f);
    std::fclose(f);
    std::string s(buf, n);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    return s;
}

std::uint64_t linkSpeedBps(const std::string& iface) {
    // Reported in Mb/s, and -1 (or an EINVAL read) for a link with no
    // meaningful speed -- a bridge, or a NIC with no carrier.
    const std::string s = readSysfs(iface, "speed");
    if (s.empty()) return 0;
    const long long mbps = std::strtoll(s.c_str(), nullptr, 10);
    if (mbps <= 0) return 0;
    return std::uint64_t(mbps) * 1000000ull;
}

// Hyphenated lower case, which is the form NMOS uses for a node's interface
// chassis_id/port_id and RFC 7273 for ts-refclk:localmac.
std::string macOf(const std::string& iface) {
    std::string s = readSysfs(iface, "address");   // "aa:bb:cc:dd:ee:ff"
    if (s.size() != 17) return {};
    for (char& c : s) {
        if (c == ':') c = '-';
        else c = char(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

// A bridge has a directory named "bridge"; a virtual device has no "device"
// symlink back to a real bus. Between them these catch docker0, br-*, virbr*
// and veth pairs even when the name convention does not.
bool looksVirtualBySysfs(const std::string& iface) {
    const std::string path = "/sys/class/net/" + iface + "/device";
    return access(path.c_str(), F_OK) != 0;
}

#endif   // !_WIN32

}  // namespace

std::string NetInterface::displayName() const {
    // Loopback does NOT avoid the local-subscriber penalty -- measured at
    // ~125 us/datagram either way. It is offered for connectivity testing only.
    if (loopback) return "Loopback " + ipv4 + " - low rate only, see docs";
    std::string s = name.empty() ? description : name;
    if (!ipv4.empty()) s += " - " + ipv4;
    const std::string sp = speedText(speedBps);
    if (!sp.empty()) s += " (" + sp + ")";
    if (!up) s += " [down]";
    return s;
}

#ifdef _WIN32

namespace {
bool looksVirtual(const std::string& name, const std::string& desc, IFTYPE ifType) {
    if (ifType == IF_TYPE_SOFTWARE_LOOPBACK || ifType == IF_TYPE_TUNNEL) return true;
    return looksVirtualByName(name, desc);
}
}  // namespace

std::vector<NetInterface> enumerateInterfaces(bool includeDown) {
    std::vector<NetInterface> out;

    ULONG size = 16 * 1024;
    std::unique_ptr<std::uint8_t[]> buf;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 4 && rc == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buf = std::make_unique<std::uint8_t[]>(size);
        rc = GetAdaptersAddresses(
            AF_INET,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr,
            reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.get()),
            &size);
    }
    if (rc != NO_ERROR) return out;

    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.get()); a; a = a->Next) {
        const bool up = a->OperStatus == IfOperStatusUp;
        const bool loopback = a->IfType == IF_TYPE_SOFTWARE_LOOPBACK;
        const bool mcast = (a->Flags & IP_ADAPTER_NO_MULTICAST) == 0;
        // Loopback is kept: it is the only way to run the sender and receiver
        // on one machine at line rate. Sending to a group that this host has
        // joined costs ~125 us per datagram through a physical NIC (8k/s) but
        // runs at 328k/s through loopback. It sorts last so it is never the
        // default.
        if (!includeDown && (!up || !mcast)) continue;

        for (auto* ua = a->FirstUnicastAddress; ua; ua = ua->Next) {
            if (!ua->Address.lpSockaddr || ua->Address.lpSockaddr->sa_family != AF_INET)
                continue;
            const auto* sin = reinterpret_cast<const sockaddr_in*>(ua->Address.lpSockaddr);

            NetInterface ni;
            ni.name        = wideToUtf8(a->FriendlyName);
            ni.description = wideToUtf8(a->Description);
            ni.index       = a->IfIndex;
            ni.speedBps    = a->TransmitLinkSpeed == static_cast<ULONG64>(-1)
                                 ? 0 : a->TransmitLinkSpeed;
            ni.up          = up;
            ni.loopback    = loopback;
            ni.multicastCapable = mcast;
            ni.virtualAdapter = looksVirtual(ni.name, ni.description, a->IfType);
            ni.ipv4Be      = sin->sin_addr.S_un.S_addr;

            if (a->PhysicalAddressLength == 6) {
                char mac[18];
                std::snprintf(mac, sizeof mac, "%02x-%02x-%02x-%02x-%02x-%02x",
                              a->PhysicalAddress[0], a->PhysicalAddress[1],
                              a->PhysicalAddress[2], a->PhysicalAddress[3],
                              a->PhysicalAddress[4], a->PhysicalAddress[5]);
                ni.mac = mac;
            }

            char ip[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof ip);
            ni.ipv4 = ip;

            out.push_back(std::move(ni));
        }
    }
    sortInterfaces(out);
    return out;
}

#else   // POSIX

std::vector<NetInterface> enumerateInterfaces(bool includeDown) {
    std::vector<NetInterface> out;

    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return out;

    for (ifaddrs* a = list; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET) continue;

        const std::string iface = a->ifa_name ? a->ifa_name : "";
        const bool up       = (a->ifa_flags & IFF_UP) && (a->ifa_flags & IFF_RUNNING);
        const bool loopback = (a->ifa_flags & IFF_LOOPBACK) != 0;
        // IFF_MULTICAST is not set on the loopback device, but multicast over
        // loopback works and is the only way to run a sender and a receiver on
        // one machine at line rate -- so loopback is admitted regardless.
        const bool mcast    = (a->ifa_flags & IFF_MULTICAST) != 0 || loopback;
        if (!includeDown && (!up || !mcast)) continue;

        const auto* sin = reinterpret_cast<const sockaddr_in*>(a->ifa_addr);

        NetInterface ni;
        ni.name        = iface;
        ni.description = loopback ? "Loopback" : readSysfs(iface, "device/modalias");
        // modalias is a driver string, not a product name; when it is there at
        // all it is more noise than description, so only the interface name is
        // relied on for display.
        ni.description.clear();
        ni.index       = if_nametoindex(iface.c_str());
        ni.speedBps    = loopback ? 0 : linkSpeedBps(iface);
        ni.up          = up;
        ni.loopback    = loopback;
        ni.multicastCapable = mcast;
        ni.virtualAdapter = !loopback && (looksVirtualByName(iface, {}) ||
                                          looksVirtualBySysfs(iface));
        ni.ipv4Be      = sin->sin_addr.s_addr;
        ni.mac         = loopback ? std::string() : macOf(iface);

        char ip[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof ip);
        ni.ipv4 = ip;

        out.push_back(std::move(ni));
    }
    freeifaddrs(list);

    sortInterfaces(out);
    return out;
}

#endif

void sortInterfaces(std::vector<NetInterface>& out) {
    // Up first, then physical before virtual, then fastest. A Hyper-V vSwitch
    // reports the same 10 Gb/s as the NIC it sits on and a docker bridge reports
    // no speed at all but is otherwise indistinguishable, so speed alone would
    // let either become the default -- and multicast out of the wrong interface
    // fails silently.
    std::stable_sort(out.begin(), out.end(), [](const NetInterface& a, const NetInterface& b) {
        if (a.up != b.up) return a.up;
        if (a.loopback != b.loopback) return !a.loopback;
        if (a.virtualAdapter != b.virtualAdapter) return !a.virtualAdapter;
        return a.speedBps > b.speedBps;
    });

    // Guarantee a loopback entry even if the pseudo-interface is not enumerated,
    // so same-machine testing is always selectable.
    const bool haveLoopback = std::any_of(out.begin(), out.end(),
        [](const NetInterface& n) { return n.loopback; });
    if (!haveLoopback) {
        NetInterface lo;
        lo.name = "Loopback";
        lo.description = "Loopback Pseudo-Interface";
        lo.ipv4 = "127.0.0.1";
        lo.ipv4Be = htonl(INADDR_LOOPBACK);
        lo.up = true;
        lo.loopback = true;
        lo.multicastCapable = true;
        out.push_back(std::move(lo));
    }
}

bool findInterfaceByIp(const std::string& ipv4, NetInterface& out) {
    for (const auto& ni : enumerateInterfaces(true))
        if (ni.ipv4 == ipv4) { out = ni; return true; }
    return false;
}

bool findInterfaceByName(const std::string& name, NetInterface& out) {
    for (const auto& ni : enumerateInterfaces(true))
        if (ni.name == name) { out = ni; return true; }
    return false;
}

bool parseIpv4(const std::string& s, std::uint32_t& beOut) {
    in_addr a{};
    if (inet_pton(AF_INET, s.c_str(), &a) != 1) return false;
    beOut = a.s_addr;
    return true;
}

bool isValidMulticastGroup(const std::string& ipv4) {
    std::uint32_t be = 0;
    if (!parseIpv4(ipv4, be)) return false;
    const std::uint32_t host = ntohl(be);
    // 224.0.0.0/4, excluding the link-local control block 224.0.0.0/24 which
    // routers never forward.
    if ((host & 0xF0000000u) != 0xE0000000u) return false;
    if ((host & 0xFFFFFF00u) == 0xE0000000u) return false;
    return true;
}

}  // namespace pcapreplay
