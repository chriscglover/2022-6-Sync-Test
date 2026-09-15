// DNS-SD browse and advertise.
//
// Dependency-free on both platforms, and deliberately so: the deployment is one
// binary with nothing to install beside it. Windows uses
// DnsServiceBrowse/Resolve/Register from dnsapi.dll (Windows 10 1703 and later)
// rather than Apple's Bonjour SDK, which is a gated download and a separate
// runtime install on every machine. Linux has no libc equivalent to call at all,
// so mdns_posix.cpp speaks multicast DNS itself rather than linking Avahi's
// client library and requiring avahi-daemon and D-Bus to be alive -- which they
// are not in a container without extra work, and a container is where most of
// these run. Both coexist with whatever mDNS stack the machine already has.
//
// Two directions, both of which NMOS needs:
//
//   browse    _nmos-register._tcp and _nmos-registration._tcp -- find the
//             registry to register with. This is the normal path, and the reason
//             the host/port override exists is that plenty of real networks
//             block mDNS across subnets.
//
//             Both types have to be browsed. IS-04 used _nmos-registration._tcp
//             up to v1.2 and added the shorter _nmos-register._tcp at v1.3,
//             because the longer name breaks the 16-character service name limit
//             in RFC 6763 section 7.2. sony/nmos-cpp advertises whichever
//             matches the API versions the registry is configured for, so a
//             registry serving v1.2 and below advertises only the long name and
//             browsing for the short one alone finds nothing at all.
//
//   advertise _nmos-node._tcp      -- peer-to-peer operation. With no registry
//             on the network at all, a controller that browses for nodes
//             directly can still find this sender and route it.
//
// Everything here is best-effort: on a network with no mDNS, or if the DNS
// client service refuses the registration, the caller carries on with whatever
// override it was given. Failures are reported, never fatal.
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace pcapreplay::nmos {

struct MdnsService {
    std::string instance;        // "registry._nmos-register._tcp.local"
    std::string displayName;     // the instance label alone
    std::string serviceType;     // "_nmos-register._tcp", parsed from `instance`
    std::string hostName;        // "server.local"
    std::string address;         // resolved IPv4, dotted quad
    std::uint16_t port = 0;
    std::map<std::string, std::string> txt;

    // Parsed out of the TXT record, per IS-04's DNS-SD binding.
    int         priority = 100;              // "pri"; lower wins
    std::string apiProto = "http";           // "api_proto"
    std::string apiAuth  = "false";          // "api_auth"
    std::vector<std::string> apiVersions;    // "api_ver", e.g. v1.2,v1.3

    bool supportsVersion(const std::string& v) const;
    std::string baseUrl(const std::string& apiVersion) const;
};

// Browses continuously and keeps a live list; NMOS registries come and go and a
// one-shot query would miss one that appears a second later.
class MdnsBrowser {
public:
    MdnsBrowser() = default;
    ~MdnsBrowser();
    MdnsBrowser(const MdnsBrowser&) = delete;
    MdnsBrowser& operator=(const MdnsBrowser&) = delete;

    // serviceType is the bare DNS-SD type, e.g. "_nmos-register._tcp".
    bool start(const std::string& serviceType);
    // Browse several types at once and pool the results. Succeeds if any one
    // browse started; `error()` then describes the ones that did not.
    bool start(const std::vector<std::string>& serviceTypes);
    void stop();
    bool running() const { return running_; }

    std::vector<MdnsService> found() const;

    // Highest-priority resolved service supporting `apiVersion`, if any.
    bool best(const std::string& apiVersion, MdnsService& out) const;

    // Why best() found nothing, when something was nonetheless discovered --
    // typically a registry whose api_ver does not list the version we ask for.
    // Empty when best() succeeded or when nothing was discovered at all.
    std::string rejection(const std::string& apiVersion) const;

    // The types passed to start(), for reporting what was actually browsed.
    std::vector<std::string> serviceTypes() const;

    std::string error() const;

private:
    struct Impl;
    void onInstance(MdnsService s);
    // A service withdrawn from the link -- an mDNS goodbye, RFC 6762 10.1.
    // Without this the published list only ever grows, and a registry that has
    // gone stays a candidate for best() indefinitely.
    void onInstanceGone(const std::string& instance);
    void onError(const std::string& e);

    mutable std::mutex       mutex_;
    std::vector<MdnsService> found_;
    std::vector<std::string> serviceTypes_;
    std::string              error_;
    bool                     running_ = false;
    Impl*                    impl_ = nullptr;
};

// The two DNS-SD types an IS-04 registry may advertise its Registration API as.
// Browse both: which one a registry uses depends on the API versions it serves.
std::vector<std::string> registryServiceTypes();

// Advertises one service instance for as long as it is alive.
class MdnsAdvertiser {
public:
    MdnsAdvertiser() = default;
    ~MdnsAdvertiser();
    MdnsAdvertiser(const MdnsAdvertiser&) = delete;
    MdnsAdvertiser& operator=(const MdnsAdvertiser&) = delete;

    // `instanceLabel` is the human-readable name, e.g. "PCAP Replay".
    // `hostIpv4` is the address to publish; empty uses the local hostname.
    bool start(const std::string& instanceLabel,
               const std::string& serviceType,
               std::uint16_t port,
               const std::map<std::string, std::string>& txt,
               const std::string& hostIpv4 = {});
    void stop();

    bool running() const { return running_; }
    std::string error() const;

private:
    struct Impl;
    Impl*       impl_ = nullptr;
    bool        running_ = false;
    mutable std::mutex mutex_;
    std::string error_;
};

// Local host label without the domain, e.g. "EDIT-1". Used to build unique
// instance names and the node's hostname.
std::string localHostLabel();

}  // namespace pcapreplay::nmos
