// DNS-SD on Linux, spoken directly rather than through a daemon.
//
// Windows hands this job to dnsapi.dll. Linux has no equivalent in libc: the
// only ways to do DNS-SD are to link Avahi's client library and require
// avahi-daemon at runtime, or to speak multicast DNS oneself. This project takes
// the second, for exactly the reason it does not link Bonjour on Windows -- the
// deployment is one binary with nothing to install alongside it, and an NMOS
// node that will not start because a daemon is missing is a node that will not
// start in a container, which is where most of them run.
//
// What is implemented, from RFC 6762 (mDNS) and RFC 6763 (DNS-SD):
//
//   browse     PTR queries for a service type, then SRV/TXT/A follow-ups for
//              each instance that comes back, with the exponential back-off
//              RFC 6762 section 5.2 asks of a continuous query. Answers are
//              taken from the additional section when a responder was kind
//              enough to include them, which most are, and asked for
//              explicitly when it was not.
//
//   advertise  a responder for one service instance: PTR for the type and for
//              the service enumeration name, SRV and TXT for the instance, and
//              an A record for the target host. Announced unsolicited twice at
//              startup per section 8.3, and withdrawn with a TTL 0 goodbye at
//              shutdown per section 10.1, so a controller does not keep a dead
//              node in its list.
//
// Coexisting with avahi-daemon. Almost every Linux box worth running this on
// already has one, holding port 5353. That is fine and expected: the socket is
// opened with SO_REUSEADDR and SO_REUSEPORT, and the kernel delivers each
// multicast datagram to every socket joined to the group, so both processes see
// all the traffic and neither interferes with the other. If 5353 cannot be
// shared at all, browsing falls back to an ephemeral port and the unicast-
// response bit, which still finds registries; advertising cannot, and says so.
//
// What is deliberately not implemented: probing and conflict resolution
// (sections 8.1 and 9), known-answer suppression, and duplicate-answer
// suppression. Probing guards against two hosts claiming one name, and every
// name published here already carries this machine's host name and node port --
// the same thing that makes the NMOS resource UUIDs unique. The suppressions are
// politeness optimisations for a busy link; one service instance and a query
// every minute at steady state is not what they exist to protect against.
#include "pcapreplay/nmos/mdns.h"

#ifndef _WIN32

#include "mdns_internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <thread>

namespace pcapreplay::nmos {
namespace {

constexpr const char*  kGroup = "224.0.0.251";
constexpr std::uint16_t kPort = 5353;

constexpr std::uint16_t kTypeA   = 1;
constexpr std::uint16_t kTypePTR = 12;
constexpr std::uint16_t kTypeTXT = 16;
constexpr std::uint16_t kTypeSRV = 33;
constexpr std::uint16_t kTypeANY = 255;

constexpr std::uint16_t kClassIN = 1;
// Top bit of the class field. In an answer it means "this is the whole truth
// about this name, drop what you had"; in a question it means "answer me
// directly rather than to the group".
constexpr std::uint16_t kCacheFlush      = 0x8000;
constexpr std::uint16_t kUnicastResponse = 0x8000;
constexpr std::uint16_t kClassMask       = 0x7FFF;

// RFC 6763 section 11 recommends 75 minutes for the records that name a service
// and 120 seconds for those that resolve it, so a host that goes away without a
// goodbye is forgotten quickly while the service listing stays stable.
constexpr std::uint32_t kTtlService = 4500;
constexpr std::uint32_t kTtlHost    = 120;

using Clock = std::chrono::steady_clock;

double secondsSince(Clock::time_point t) {
    return std::chrono::duration<double>(Clock::now() - t).count();
}

// ---------------------------------------------------------------------------
// Interfaces
// ---------------------------------------------------------------------------

struct Iface {
    std::string   name;
    unsigned      index = 0;
    std::uint32_t addrBe = 0;      // network byte order
    bool          loopback = false;
};

// Every IPv4 interface that could carry multicast DNS.
//
// All of them are joined and queried on, not just the one the node API binds:
// the registry is frequently on a different link from the media network, and a
// browse that only looked at the media NIC would miss it. Duplicate answers
// arriving on two interfaces are harmless -- instances are keyed by name.
std::vector<Iface> multicastInterfaces() {
    std::vector<Iface> out;
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return out;
    for (ifaddrs* a = list; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET) continue;
        if (!(a->ifa_flags & IFF_UP) || !(a->ifa_flags & IFF_RUNNING)) continue;
        const bool loopback = (a->ifa_flags & IFF_LOOPBACK) != 0;
        if (!loopback && !(a->ifa_flags & IFF_MULTICAST)) continue;
        Iface i;
        i.name     = a->ifa_name ? a->ifa_name : "";
        i.index    = if_nametoindex(i.name.c_str());
        i.addrBe   = reinterpret_cast<const sockaddr_in*>(a->ifa_addr)->sin_addr.s_addr;
        i.loopback = loopback;
        out.push_back(std::move(i));
    }
    freeifaddrs(list);
    return out;
}

std::string ipText(std::uint32_t addrBe) {
    in_addr a{};
    a.s_addr = addrBe;
    char buf[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &a, buf, sizeof buf);
    return buf;
}

// ---------------------------------------------------------------------------
// Message encoding
// ---------------------------------------------------------------------------

struct Writer {
    std::vector<std::uint8_t> b;

    void u8(std::uint8_t v)   { b.push_back(v); }
    void u16(std::uint16_t v) { b.push_back(std::uint8_t(v >> 8)); b.push_back(std::uint8_t(v)); }
    void u32(std::uint32_t v) {
        b.push_back(std::uint8_t(v >> 24)); b.push_back(std::uint8_t(v >> 16));
        b.push_back(std::uint8_t(v >> 8));  b.push_back(std::uint8_t(v));
    }
    void bytes(const void* p, std::size_t n) {
        const auto* q = static_cast<const std::uint8_t*>(p);
        b.insert(b.end(), q, q + n);
    }

    // Names are written out in full rather than compressed. Compression is an
    // optimisation for a name repeated within one message; every message here is
    // a few hundred bytes and well inside the 1500 an mDNS packet may be, so the
    // clarity is worth more than the bytes. Readers must still handle compressed
    // names in what arrives, and do.
    void labels(const std::vector<std::string>& ls) {
        for (const std::string& l : ls) {
            const std::size_t n = std::min<std::size_t>(l.size(), 63);
            u8(std::uint8_t(n));
            bytes(l.data(), n);
        }
        u8(0);
    }

    // A dotted name where no label contains a dot -- service types, host names.
    void name(const std::string& dotted) {
        std::vector<std::string> ls;
        std::string cur;
        for (char c : dotted) {
            if (c == '.') { if (!cur.empty()) ls.push_back(cur); cur.clear(); }
            else cur += c;
        }
        if (!cur.empty()) ls.push_back(cur);
        labels(ls);
    }

    void question(const std::string& n, std::uint16_t type, std::uint16_t cls) {
        name(n);
        u16(type);
        u16(cls);
    }

    // Record header, returning where the rdata length has to be back-filled once
    // the rdata itself has been written -- which is the only way to write a
    // record whose rdata is a name of unknown encoded length.
    std::size_t recordHead(const std::vector<std::string>& n, std::uint16_t type,
                           std::uint16_t cls, std::uint32_t ttl) {
        labels(n);
        u16(type);
        u16(cls);
        u32(ttl);
        const std::size_t at = b.size();
        u16(0);
        return at;
    }
    void patchLength(std::size_t at) {
        const std::uint16_t n = std::uint16_t(b.size() - at - 2);
        b[at]     = std::uint8_t(n >> 8);
        b[at + 1] = std::uint8_t(n);
    }
};

// ---------------------------------------------------------------------------
// Message decoding
// ---------------------------------------------------------------------------

struct Record {
    std::string   name;
    std::uint16_t type = 0;
    std::uint16_t cls  = 0;
    std::uint32_t ttl  = 0;
    std::size_t   rdataAt = 0;
    std::size_t   rdataLen = 0;
};

struct Message {
    std::uint16_t flags = 0;
    struct Question { std::string name; std::uint16_t type = 0, cls = 0; };
    std::vector<Question> questions;
    std::vector<Record>   records;      // answers, authority and additional
    bool isResponse() const { return (flags & 0x8000) != 0; }
};

class Parser {
public:
    Parser(const std::uint8_t* p, std::size_t n) : p_(p), n_(n) {}

    bool u16(std::uint16_t& v) {
        if (at_ + 2 > n_) return false;
        v = std::uint16_t((p_[at_] << 8) | p_[at_ + 1]);
        at_ += 2;
        return true;
    }
    bool u32(std::uint32_t& v) {
        if (at_ + 4 > n_) return false;
        v = (std::uint32_t(p_[at_]) << 24) | (std::uint32_t(p_[at_ + 1]) << 16) |
            (std::uint32_t(p_[at_ + 2]) << 8) | std::uint32_t(p_[at_ + 3]);
        at_ += 4;
        return true;
    }

    // Reads a possibly-compressed name and leaves `at_` after it.
    bool name(std::string& out) { return nameAt(at_, out, &at_); }

    // Reads a name found at an arbitrary offset -- rdata of a PTR or the target
    // of an SRV -- without disturbing the cursor.
    bool nameAt(std::size_t from, std::string& out, std::size_t* advance = nullptr) const {
        out.clear();
        std::size_t here = from;
        bool jumped = false;
        int hops = 0;
        for (;;) {
            if (here >= n_) return false;
            const std::uint8_t len = p_[here];
            if ((len & 0xC0) == 0xC0) {
                // A compression pointer. Bounded hops, because a message from
                // the network can point a name at itself and a naive reader
                // spins for ever on it.
                if (here + 1 >= n_) return false;
                const std::size_t target =
                    std::size_t((len & 0x3F) << 8) | std::size_t(p_[here + 1]);
                if (!jumped && advance) { *advance = here + 2; jumped = true; }
                if (++hops > 16) return false;
                here = target;
                continue;
            }
            if ((len & 0xC0) != 0) return false;          // reserved encoding
            if (len == 0) {
                if (!jumped && advance) *advance = here + 1;
                return true;
            }
            if (here + 1 + len > n_) return false;
            if (!out.empty()) out += '.';
            out.append(reinterpret_cast<const char*>(p_ + here + 1), len);
            here += 1 + len;
        }
    }

    const std::uint8_t* base() const { return p_; }
    std::size_t size() const { return n_; }
    std::size_t at() const { return at_; }
    void seek(std::size_t at) { at_ = at; }

private:
    const std::uint8_t* p_;
    std::size_t n_;
    std::size_t at_ = 0;
};

bool parseMessage(const std::uint8_t* p, std::size_t n, Message& out, Parser& parser) {
    parser = Parser(p, n);
    std::uint16_t id = 0, qd = 0, an = 0, ns = 0, ar = 0;
    if (!parser.u16(id) || !parser.u16(out.flags) || !parser.u16(qd) ||
        !parser.u16(an) || !parser.u16(ns) || !parser.u16(ar))
        return false;

    for (std::uint16_t i = 0; i < qd; ++i) {
        Message::Question q;
        if (!parser.name(q.name) || !parser.u16(q.type) || !parser.u16(q.cls))
            return false;
        out.questions.push_back(std::move(q));
    }

    const std::uint32_t total = std::uint32_t(an) + ns + ar;
    for (std::uint32_t i = 0; i < total; ++i) {
        Record r;
        std::uint16_t rdlen = 0;
        if (!parser.name(r.name) || !parser.u16(r.type) || !parser.u16(r.cls) ||
            !parser.u32(r.ttl) || !parser.u16(rdlen))
            return false;
        r.rdataAt  = parser.at();
        r.rdataLen = rdlen;
        if (r.rdataAt + rdlen > n) return false;
        parser.seek(r.rdataAt + rdlen);
        out.records.push_back(std::move(r));
    }
    return true;
}

// TXT rdata is a run of length-prefixed strings, each conventionally "key=value".
// A string with no '=' is a key with an empty value, which is what the DNS-SD
// spec says and what "api_auth" alone would mean.
std::map<std::string, std::string> parseTxt(const std::uint8_t* p, std::size_t at,
                                            std::size_t len) {
    std::map<std::string, std::string> out;
    const std::size_t end = at + len;
    while (at < end) {
        const std::size_t n = p[at++];
        if (at + n > end) break;
        const std::string entry(reinterpret_cast<const char*>(p + at), n);
        at += n;
        if (entry.empty()) continue;
        const std::size_t eq = entry.find('=');
        if (eq == std::string::npos) out[entry] = std::string();
        else                         out[entry.substr(0, eq)] = entry.substr(eq + 1);
    }
    return out;
}

// ---------------------------------------------------------------------------
// The socket
// ---------------------------------------------------------------------------

// One socket, shared by the browser and the responder in whichever object owns
// it. Bound to 5353 where it can be, so responses sent to the group are seen;
// on an ephemeral port where it cannot, in which case queries have to ask for a
// unicast reply.
class MdnsSocket {
public:
    ~MdnsSocket() { close(); }

    bool open(bool needPort5353, std::string& error) {
        close();
        fd_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) { error = "socket: " + std::string(std::strerror(errno)); return false; }

        const int on = 1;
        setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
#ifdef SO_REUSEPORT
        // Both are needed to share 5353 with avahi-daemon: REUSEADDR alone lets
        // the bind succeed on some kernels and not others, and REUSEPORT is what
        // actually makes the kernel deliver each multicast datagram to every
        // joined socket rather than just one of them.
        setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &on, sizeof on);
#endif

        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        local.sin_port = htons(kPort);
        if (bind(fd_, reinterpret_cast<const sockaddr*>(&local), sizeof local) != 0) {
            const int e = errno;
            if (needPort5353) {
                error = "cannot bind UDP 5353: " + std::string(std::strerror(e));
                ::close(fd_);
                fd_ = -1;
                return false;
            }
            // Fall back to an ephemeral port. Responses to a query from one are
            // only delivered if the query asked for a unicast reply, which
            // queries then do -- see sendQuery().
            local.sin_port = 0;
            if (bind(fd_, reinterpret_cast<const sockaddr*>(&local), sizeof local) != 0) {
                error = "cannot bind a UDP port for mDNS: " +
                        std::string(std::strerror(errno));
                ::close(fd_);
                fd_ = -1;
                return false;
            }
            onPort5353_ = false;
            error = "UDP 5353 is in use and could not be shared (" +
                    std::string(std::strerror(e)) +
                    "); querying from a private port instead";
        } else {
            onPort5353_ = true;
        }

        // 255, per RFC 6762 section 11 -- link-local by address, and a TTL of 255
        // is what lets a receiver verify the packet was not routed.
        const int ttl = 255;
        setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
        const int loop = 1;      // so two instances on one host can see each other
        setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof loop);

        interfaces_ = multicastInterfaces();
        int joined = 0;
        for (const Iface& i : interfaces_) {
            ip_mreqn mreq{};
            inet_pton(AF_INET, kGroup, &mreq.imr_multiaddr);
            mreq.imr_address.s_addr = i.addrBe;
            mreq.imr_ifindex = int(i.index);
            if (setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof mreq) == 0)
                ++joined;
        }
        if (joined == 0) {
            error = "could not join 224.0.0.251 on any interface";
            ::close(fd_);
            fd_ = -1;
            return false;
        }
        setNonBlocking();
        return true;
    }

    void close() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
        interfaces_.clear();
    }

    bool isOpen() const { return fd_ >= 0; }
    bool onPort5353() const { return onPort5353_; }
    const std::vector<Iface>& interfaces() const { return interfaces_; }

    // Out of every interface, because which one reaches the registry is not
    // knowable here and sending on the wrong one is a silent failure.
    void sendToGroup(const std::vector<std::uint8_t>& msg) {
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(kPort);
        inet_pton(AF_INET, kGroup, &to.sin_addr);

        for (const Iface& i : interfaces_) {
            ip_mreqn req{};
            req.imr_address.s_addr = i.addrBe;
            req.imr_ifindex = int(i.index);
            if (setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_IF, &req, sizeof req) != 0)
                continue;
            sendto(fd_, msg.data(), msg.size(), 0,
                   reinterpret_cast<const sockaddr*>(&to), sizeof to);
        }
    }

    void sendTo(const std::vector<std::uint8_t>& msg, const sockaddr_in& to) {
        sendto(fd_, msg.data(), msg.size(), 0,
               reinterpret_cast<const sockaddr*>(&to), sizeof to);
    }

    // Waits up to timeoutMs for a datagram. Returns its length, 0 on timeout,
    // -1 on error.
    int receive(std::uint8_t* buf, std::size_t cap, sockaddr_in& from, int timeoutMs) {
        pollfd pfd{fd_, POLLIN, 0};
        const int r = poll(&pfd, 1, timeoutMs);
        if (r <= 0) return r == 0 ? 0 : -1;
        socklen_t len = sizeof from;
        const ssize_t n = recvfrom(fd_, buf, cap, 0,
                                   reinterpret_cast<sockaddr*>(&from), &len);
        if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
        return int(n);
    }

private:
    void setNonBlocking() {
        const int flags = fcntl(fd_, F_GETFL, 0);
        if (flags >= 0) fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
    }

    int  fd_ = -1;
    bool onPort5353_ = false;
    std::vector<Iface> interfaces_;
};

}  // namespace

// ---------------------------------------------------------------------------

std::string localHostLabel() {
    char buf[256] = {};
    if (gethostname(buf, sizeof buf - 1) != 0) return "pcap-replay";
    std::string s = buf;
    // Just the label: a machine configured with a search domain reports
    // "host.example.com" here, and the NMOS resource seed and the DNS-SD
    // instance name both want the short form.
    const std::size_t dot = s.find('.');
    if (dot != std::string::npos) s.erase(dot);
    return s.empty() ? "pcap-replay" : s;
}

// ---------------------------------------------------------------------------
// Browser
// ---------------------------------------------------------------------------

struct MdnsBrowser::Impl {
    MdnsBrowser* owner = nullptr;
    std::vector<std::string> types;          // fully qualified, with .local
    MdnsSocket   sock;
    std::thread  thread;
    std::atomic<bool> stopping{false};

    // What has been learnt so far, per instance. An instance is published to the
    // owner as soon as it has an SRV -- that is the point at which it has a host
    // and a port and could in principle be used -- and re-published as the TXT
    // and the address fill in.
    struct Entry {
        MdnsService svc;
        bool haveSrv = false;
        bool haveTxt = false;
        Clock::time_point lastAsked{};
    };
    std::map<std::string, Entry>       instances;   // keyed by full instance name
    std::map<std::string, std::string> addrByHost;  // "registry.local" -> dotted quad

    void run();
    void sendQueries(const std::vector<std::pair<std::string, std::uint16_t>>& qs);
    void handle(const std::uint8_t* p, std::size_t n);
    void chaseMissing();
    void publish(Entry& e);
};

// One message carrying every question, which is legal and is what a single
// browse ought to look like on the wire.
void MdnsBrowser::Impl::sendQueries(
        const std::vector<std::pair<std::string, std::uint16_t>>& qs) {
    if (qs.empty() || !sock.isOpen()) return;
    Writer w;
    w.u16(0);                                   // transaction id: always 0 in mDNS
    w.u16(0);                                   // flags: a standard query
    w.u16(std::uint16_t(qs.size()));            // QDCOUNT
    w.u16(0); w.u16(0); w.u16(0);
    // Ask for a unicast reply only when we are not on 5353: a responder answers
    // to the group by default, and a socket on a private port would never see
    // it. On 5353 the multicast answer arrives normally, and asking for unicast
    // there would deprive every other listener of the answer.
    const std::uint16_t cls =
        kClassIN | (sock.onPort5353() ? 0 : kUnicastResponse);
    for (const auto& q : qs) w.question(q.first, q.second, cls);
    sock.sendToGroup(w.b);
}

void MdnsBrowser::Impl::publish(Entry& e) {
    MdnsService s = e.svc;
    if (s.address.empty()) {
        const auto it = addrByHost.find(s.hostName);
        if (it != addrByHost.end()) s.address = it->second;
    }
    detail::deriveNames(s);
    detail::applyTxtRecords(s);
    owner->onInstance(std::move(s));
}

void MdnsBrowser::Impl::handle(const std::uint8_t* p, std::size_t n) {
    Message msg;
    Parser parser(p, n);
    if (!parseMessage(p, n, msg, parser)) return;
    if (!msg.isResponse()) return;              // someone else's query

    bool changed = false;

    for (const Record& r : msg.records) {
        const std::uint16_t cls = r.cls & kClassMask;
        if (cls != kClassIN) continue;

        switch (r.type) {
        case kTypePTR: {
            // Is it one of the types being browsed for? A response can carry
            // records for anything at all, and on a link with avahi on it,
            // usually does.
            const bool wanted = std::find(types.begin(), types.end(), r.name) !=
                                types.end();
            if (!wanted) break;
            std::string instance;
            if (!parser.nameAt(r.rdataAt, instance) || instance.empty()) break;
            // A goodbye. The instance is going away, so forget it rather than
            // leaving a dead registry in the list for the next 75 minutes.
            //
            // It has to come out of the owner's published list as well as this
            // one: that list is what found() returns and what best() chooses
            // from, so dropping it here alone would leave a departed registry
            // as a candidate for ever.
            if (r.ttl == 0) {
                instances.erase(instance);
                addrByHost.erase(instance);
                owner->onInstanceGone(instance);
                changed = true;
                break;
            }
            if (instances.find(instance) == instances.end()) {
                Entry e;
                e.svc.instance = instance;
                instances.emplace(instance, std::move(e));
                changed = true;
            }
            break;
        }
        case kTypeSRV: {
            const auto it = instances.find(r.name);
            if (it == instances.end()) break;
            if (r.rdataLen < 7) break;
            const std::uint8_t* rd = p + r.rdataAt;
            const std::uint16_t port = std::uint16_t((rd[4] << 8) | rd[5]);
            std::string target;
            if (!parser.nameAt(r.rdataAt + 6, target)) break;
            it->second.svc.port     = port;
            it->second.svc.hostName = target;
            it->second.haveSrv      = true;
            changed = true;
            break;
        }
        case kTypeTXT: {
            const auto it = instances.find(r.name);
            if (it == instances.end()) break;
            it->second.svc.txt = parseTxt(p, r.rdataAt, r.rdataLen);
            it->second.haveTxt = true;
            changed = true;
            break;
        }
        case kTypeA: {
            if (r.rdataLen != 4 || r.ttl == 0) break;
            std::uint32_t be = 0;
            std::memcpy(&be, p + r.rdataAt, 4);
            addrByHost[r.name] = ipText(be);
            changed = true;
            break;
        }
        default:
            break;
        }
    }

    if (!changed) return;
    for (auto& kv : instances)
        if (kv.second.haveSrv) publish(kv.second);
}

// Ask directly for whatever a responder did not volunteer.
//
// A well-behaved responder puts the SRV, TXT and A records in the additional
// section of its PTR answer, and then there is nothing to do here. Not all do,
// and a registry that is listed but never resolves is indistinguishable to the
// user from one that is not there at all -- so anything still missing a second
// after it was first seen gets asked for by name.
void MdnsBrowser::Impl::chaseMissing() {
    std::vector<std::pair<std::string, std::uint16_t>> qs;
    for (auto& kv : instances) {
        Entry& e = kv.second;
        if (e.haveSrv && e.haveTxt &&
            (!e.svc.hostName.empty() && addrByHost.count(e.svc.hostName)))
            continue;
        if (e.lastAsked.time_since_epoch().count() != 0 && secondsSince(e.lastAsked) < 2.0)
            continue;
        e.lastAsked = Clock::now();

        // ANY rather than SRV and TXT separately: it is one question instead of
        // two and every responder answers it with both.
        if (!e.haveSrv || !e.haveTxt) qs.emplace_back(kv.first, kTypeANY);
        if (e.haveSrv && !e.svc.hostName.empty() && !addrByHost.count(e.svc.hostName))
            qs.emplace_back(e.svc.hostName, kTypeA);
        if (qs.size() >= 16) break;      // keep any one message sane
    }
    sendQueries(qs);
}

void MdnsBrowser::Impl::run() {
    std::vector<std::pair<std::string, std::uint16_t>> browse;
    for (const std::string& t : types) browse.emplace_back(t, kTypePTR);

    // RFC 6762 section 5.2: a continuous query starts at a one-second interval
    // and doubles, capped at an hour. Capped at a minute here instead -- a
    // registry appearing on the link is a thing this node wants to notice
    // promptly, and one query a minute is nothing.
    double interval = 1.0;
    auto nextQuery = Clock::now();
    auto nextChase = Clock::now() + std::chrono::seconds(1);

    std::vector<std::uint8_t> buf(9000);

    while (!stopping.load(std::memory_order_relaxed)) {
        const auto now = Clock::now();
        if (now >= nextQuery) {
            sendQueries(browse);
            nextQuery = now + std::chrono::milliseconds(std::int64_t(interval * 1000.0));
            interval = std::min(interval * 2.0, 60.0);
        }
        if (now >= nextChase) {
            chaseMissing();
            nextChase = now + std::chrono::seconds(2);
        }

        sockaddr_in from{};
        const int n = sock.receive(buf.data(), buf.size(), from, 200);
        if (n > 0) handle(buf.data(), std::size_t(n));
    }
}

bool MdnsBrowser::start(const std::vector<std::string>& serviceTypes) {
    stop();
    {
        std::lock_guard<std::mutex> lk(mutex_);
        found_.clear();
        error_.clear();
        serviceTypes_ = serviceTypes;
    }

    auto impl = std::make_unique<Impl>();
    impl->owner = this;
    for (const std::string& t : serviceTypes) {
        if (!t.empty()) impl->types.push_back(detail::qualify(t));
    }
    if (impl->types.empty()) {
        std::lock_guard<std::mutex> lk(mutex_);
        error_ = "cannot browse: no service type given";
        return false;
    }

    // Browsing does not need 5353: if it cannot be had, queries ask for a
    // unicast reply from a private port instead, which finds registries just as
    // well. The reason is still reported, because it is worth knowing that
    // something else on the machine owns the port.
    std::string why;
    if (!impl->sock.open(false, why)) {
        std::lock_guard<std::mutex> lk(mutex_);
        error_ = "cannot browse: " + why;
        return false;
    }
    if (!why.empty()) {
        std::lock_guard<std::mutex> lk(mutex_);
        error_ = why;
    }

    impl_ = impl.release();
    impl_->thread = std::thread([this] { impl_->run(); });
    running_ = true;
    return true;
}

void MdnsBrowser::stop() {
    if (!impl_) { running_ = false; return; }
    impl_->stopping.store(true, std::memory_order_relaxed);
    if (impl_->thread.joinable()) impl_->thread.join();
    impl_->sock.close();
    delete impl_;
    impl_ = nullptr;
    running_ = false;
}

// ---------------------------------------------------------------------------
// Advertiser
// ---------------------------------------------------------------------------

struct MdnsAdvertiser::Impl {
    MdnsAdvertiser* owner = nullptr;
    MdnsSocket   sock;
    std::thread  thread;
    std::atomic<bool> stopping{false};

    // The four names this responder is authoritative for.
    std::string typeName;        // "_nmos-node._tcp.local"
    std::string instanceName;    // "PCAP Replay host:3210._nmos-node._tcp.local"
    std::string instanceLabel;   // the label alone, as its own DNS label
    std::string hostName;        // "pcap-replay-3210.local"
    std::uint16_t port = 0;
    std::uint32_t addrBe = 0;
    std::map<std::string, std::string> txt;

    static constexpr const char* kEnumeration = "_services._dns-sd._udp.local";

    void run();
    std::vector<std::uint8_t> buildAnnouncement(std::uint32_t ttl) const;
    void appendRecords(Writer& w, std::uint32_t ttlService, std::uint32_t ttlHost,
                       bool ptr, bool srv, bool txtRec, bool a, int& count) const;
    void answer(const Message& msg, const Parser& parser, const sockaddr_in& from);
};

// The service's records, written into `w`. Which of the four are wanted depends
// on what was asked; an announcement wants all of them.
void MdnsAdvertiser::Impl::appendRecords(Writer& w, std::uint32_t ttlService,
                                         std::uint32_t ttlHost, bool ptr, bool srv,
                                         bool txtRec, bool a, int& count) const {
    // Instance and type as label vectors, because the instance label is one
    // label that legitimately contains spaces and a colon and must not be split
    // on them.
    std::vector<std::string> typeLabels;
    {
        std::string cur;
        for (char c : typeName) {
            if (c == '.') { if (!cur.empty()) typeLabels.push_back(cur); cur.clear(); }
            else cur += c;
        }
        if (!cur.empty()) typeLabels.push_back(cur);
    }
    std::vector<std::string> instanceLabels;
    instanceLabels.push_back(instanceLabel);
    instanceLabels.insert(instanceLabels.end(), typeLabels.begin(), typeLabels.end());

    std::vector<std::string> hostLabels;
    {
        std::string cur;
        for (char c : hostName) {
            if (c == '.') { if (!cur.empty()) hostLabels.push_back(cur); cur.clear(); }
            else cur += c;
        }
        if (!cur.empty()) hostLabels.push_back(cur);
    }

    if (ptr) {
        // The service type's PTR. No cache-flush bit: several instances may share
        // a type and claiming exclusivity would evict the others.
        const std::size_t at = w.recordHead(typeLabels, kTypePTR, kClassIN, ttlService);
        w.labels(instanceLabels);
        w.patchLength(at);
        ++count;

        // ... and the DNS-SD service-type enumeration, so a controller browsing
        // "what kinds of thing are on this link" sees _nmos-node._tcp at all.
        std::vector<std::string> enumLabels{"_services", "_dns-sd", "_udp", "local"};
        const std::size_t at2 = w.recordHead(enumLabels, kTypePTR, kClassIN, ttlService);
        w.labels(typeLabels);
        w.patchLength(at2);
        ++count;
    }
    if (srv) {
        const std::size_t at =
            w.recordHead(instanceLabels, kTypeSRV, kClassIN | kCacheFlush, ttlHost);
        w.u16(0);            // priority
        w.u16(0);            // weight
        w.u16(port);
        w.labels(hostLabels);
        w.patchLength(at);
        ++count;
    }
    if (txtRec) {
        const std::size_t at =
            w.recordHead(instanceLabels, kTypeTXT, kClassIN | kCacheFlush, ttlService);
        if (txt.empty()) {
            // A service with no TXT data still needs a TXT record, and it has to
            // be one empty string rather than nothing at all -- RFC 6763 6.1.
            w.u8(0);
        } else {
            for (const auto& kv : txt) {
                const std::string entry = kv.first + "=" + kv.second;
                const std::size_t n = std::min<std::size_t>(entry.size(), 255);
                w.u8(std::uint8_t(n));
                w.bytes(entry.data(), n);
            }
        }
        w.patchLength(at);
        ++count;
    }
    if (a && addrBe != 0) {
        const std::size_t at =
            w.recordHead(hostLabels, kTypeA, kClassIN | kCacheFlush, ttlHost);
        w.bytes(&addrBe, 4);
        w.patchLength(at);
        ++count;
    }
}

std::vector<std::uint8_t> MdnsAdvertiser::Impl::buildAnnouncement(std::uint32_t ttl) const {
    Writer w;
    w.u16(0);
    w.u16(0x8400);                 // response, authoritative
    w.u16(0);                      // no questions
    const std::size_t countAt = w.b.size();
    w.u16(0);                      // ANCOUNT, back-filled
    w.u16(0); w.u16(0);

    int count = 0;
    appendRecords(w, ttl, ttl ? kTtlHost : 0, true, true, true, true, count);
    w.b[countAt]     = std::uint8_t(count >> 8);
    w.b[countAt + 1] = std::uint8_t(count);
    return w.b;
}

void MdnsAdvertiser::Impl::answer(const Message& msg, const Parser&,
                                  const sockaddr_in& from) {
    bool wantPtr = false, wantSrv = false, wantTxt = false, wantA = false;
    bool unicast = false;

    for (const auto& q : msg.questions) {
        if ((q.cls & kUnicastResponse) != 0) unicast = true;
        const std::uint16_t cls = q.cls & kClassMask;
        if (cls != kClassIN && cls != 255) continue;

        const bool any = q.type == kTypeANY;
        if (q.name == typeName || q.name == kEnumeration) {
            if (any || q.type == kTypePTR) wantPtr = true;
        } else if (q.name == instanceName) {
            if (any || q.type == kTypeSRV) wantSrv = true;
            if (any || q.type == kTypeTXT) wantTxt = true;
        } else if (q.name == hostName) {
            if (any || q.type == kTypeA) wantA = true;
        }
    }
    if (!wantPtr && !wantSrv && !wantTxt && !wantA) return;

    // Anything that names the service gets the records that resolve it too,
    // in the additional section -- the whole point of which is that a browse
    // does not then need a second round trip.
    Writer w;
    w.u16(0);
    w.u16(0x8400);
    w.u16(0);
    const std::size_t answerCountAt = w.b.size();
    w.u16(0);
    w.u16(0);
    const std::size_t additionalCountAt = w.b.size();
    w.u16(0);

    int answers = 0;
    appendRecords(w, kTtlService, kTtlHost, wantPtr, wantSrv, wantTxt, wantA, answers);
    int additional = 0;
    if (wantPtr) {
        appendRecords(w, kTtlService, kTtlHost, false, !wantSrv, !wantTxt, !wantA,
                      additional);
    } else if ((wantSrv || wantTxt) && !wantA) {
        appendRecords(w, kTtlService, kTtlHost, false, false, false, true, additional);
    }

    w.b[answerCountAt]     = std::uint8_t(answers >> 8);
    w.b[answerCountAt + 1] = std::uint8_t(answers);
    w.b[additionalCountAt]     = std::uint8_t(additional >> 8);
    w.b[additionalCountAt + 1] = std::uint8_t(additional);

    if (unicast) sock.sendTo(w.b, from);
    else         sock.sendToGroup(w.b);
}

void MdnsAdvertiser::Impl::run() {
    // RFC 6762 section 8.3: announce at least twice, a second apart, so a
    // listener that missed the first still hears about the service without
    // waiting for its next scheduled query.
    sock.sendToGroup(buildAnnouncement(kTtlService));
    auto secondAnnounce = Clock::now() + std::chrono::seconds(1);
    bool announced = false;

    std::vector<std::uint8_t> buf(9000);
    while (!stopping.load(std::memory_order_relaxed)) {
        if (!announced && Clock::now() >= secondAnnounce) {
            sock.sendToGroup(buildAnnouncement(kTtlService));
            announced = true;
        }

        sockaddr_in from{};
        const int n = sock.receive(buf.data(), buf.size(), from, 200);
        if (n <= 0) continue;

        Message msg;
        Parser parser(buf.data(), std::size_t(n));
        if (!parseMessage(buf.data(), std::size_t(n), msg, parser)) continue;
        if (msg.isResponse()) continue;
        answer(msg, parser, from);
    }

    // Goodbye: the same records at TTL 0, which tells every controller on the
    // link to drop this node now rather than keep it listed until the TTL runs
    // out. A node that lingers in a controller after it has gone is worse than
    // one that never appeared.
    sock.sendToGroup(buildAnnouncement(0));
}

bool MdnsAdvertiser::start(const std::string& instanceLabel,
                           const std::string& serviceType,
                           std::uint16_t port,
                           const std::map<std::string, std::string>& txt,
                           const std::string& hostIpv4) {
    stop();
    {
        std::lock_guard<std::mutex> lk(mutex_);
        error_.clear();
    }

    auto impl = std::make_unique<Impl>();
    impl->owner         = this;
    impl->typeName      = detail::qualify(serviceType);
    impl->instanceLabel = instanceLabel;
    impl->instanceName  = instanceLabel + "." + impl->typeName;
    impl->port          = port;
    impl->txt           = txt;

    // A host name of this instance's own rather than "<hostname>.local".
    //
    // avahi-daemon, where it is running, is already authoritative for
    // "<hostname>.local" and publishes an A record for every address the machine
    // has. Publishing a second, different answer for the same name is a name
    // conflict in mDNS terms, and the correct outcome of one is that somebody
    // renames -- which is not a fight worth having for a record that exists only
    // to point an SRV somewhere. A name derived from the node port cannot
    // collide with avahi's, cannot collide with a second instance's, and
    // resolves through the A record served here.
    impl->hostName = "pcap-replay-" + localHostLabel() + "-" +
                     std::to_string(port) + ".local";
    for (char& c : impl->hostName)
        if (c == ' ' || c == ':' || c == '_') c = '-';

    if (!hostIpv4.empty()) {
        in_addr a{};
        if (inet_pton(AF_INET, hostIpv4.c_str(), &a) == 1) impl->addrBe = a.s_addr;
    }
    if (impl->addrBe == 0) {
        // No address was chosen, which is what binding every interface means.
        // Publish the first real one: an SRV target that resolves to nothing is
        // a service a controller can see and cannot reach.
        for (const Iface& i : multicastInterfaces()) {
            if (!i.loopback) { impl->addrBe = i.addrBe; break; }
        }
    }

    // Advertising genuinely does need 5353: a responder that is not on it never
    // sees the queries it exists to answer.
    std::string why;
    if (!impl->sock.open(true, why)) {
        std::lock_guard<std::mutex> lk(mutex_);
        error_ = "cannot advertise " + impl->instanceName + ": " + why +
                 " (peer-to-peer discovery unavailable; registry registration "
                 "is unaffected)";
        return false;
    }

    impl_ = impl.release();
    impl_->thread = std::thread([this] { impl_->run(); });
    running_ = true;
    return true;
}

void MdnsAdvertiser::stop() {
    if (!impl_) { running_ = false; return; }
    impl_->stopping.store(true, std::memory_order_relaxed);
    if (impl_->thread.joinable()) impl_->thread.join();   // sends the goodbye
    impl_->sock.close();
    delete impl_;
    impl_ = nullptr;
    running_ = false;
}

}  // namespace pcapreplay::nmos

#endif   // !_WIN32
