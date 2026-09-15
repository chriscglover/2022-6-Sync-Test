#include "pcapreplay/net_multicast.h"

#ifdef _WIN32
#include <mstcpip.h>
// Not exposed by every Windows SDK header, but the ioctl itself has been
// supported since Windows 2000.
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#ifndef UDP_SEND_MSG_SIZE
#define UDP_SEND_MSG_SIZE 2
#endif
#else
#include <netinet/udp.h>
// UDP_SEGMENT is the Linux generic-segmentation-offload control. Kernel 4.18
// and later; defined here so the build does not require headers that new, and
// the setsockopt is probed at runtime anyway.
#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103
#endif
#endif

#include <cstring>

namespace pcapreplay {
namespace {

constexpr std::uintptr_t kInvalid = kInvalidHandle;

socket_t asSocket(std::uintptr_t s) { return fromHandle(s); }

}  // namespace

// ---------------------------------------------------------------------------

MulticastSender::~MulticastSender() { close(); }

bool MulticastSender::open(const MulticastEndpoint& ep, int ttl, bool loopback,
                           int sendBufferBytes) {
    close();

    in_addr group{};
    if (inet_pton(AF_INET, ep.group.c_str(), &group) != 1) {
        lastError_ = "invalid multicast group '" + ep.group + "'";
        return false;
    }
    if (ep.port == 0) {
        lastError_ = "port must be non-zero";
        return false;
    }

    socket_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kInvalidSocket) {
        lastError_ = "socket: " + socketErrorText(socketError());
        return false;
    }

    // Choose the outgoing interface explicitly -- the whole point of the
    // per-path NIC selector. Without it the kernel picks by route table, and on
    // a multi-homed box multicast leaves by the wrong link with no error at all.
    if (!ep.interfaceIp.empty()) {
        in_addr ifAddr{};
        if (inet_pton(AF_INET, ep.interfaceIp.c_str(), &ifAddr) != 1) {
            lastError_ = "invalid interface address '" + ep.interfaceIp + "'";
            closeSocket(s);
            return false;
        }
        if (setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF,
                       sockoptPtr(&ifAddr), sizeof ifAddr) != 0) {
            lastError_ = "IP_MULTICAST_IF: " + socketErrorText(socketError());
            closeSocket(s);
            return false;
        }
    }

    // Both of these are a DWORD on Winsock and an int on POSIX, which are the
    // same four bytes; the kernel also accepts a single byte for the two IPv4
    // multicast options, so an int is safe on either.
    const int ttlV = ttl;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, sockoptPtr(&ttlV), sizeof ttlV);

    const int loopV = loopback ? 1 : 0;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP, sockoptPtr(&loopV), sizeof loopV);

    // A generous send buffer stops sendto() blocking on a burst; too small and
    // the pacer's careful spacing is destroyed by backpressure.
    //
    // Linux halves what it is asked for and silently clamps to
    // net.core.wmem_max, which on a stock kernel is 212 kB -- far below what is
    // wanted here. The clamp is not an error, so it is read back and reported
    // rather than assumed; see the note in bufferShortfall().
    const int snd = sendBufferBytes;
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, sockoptPtr(&snd), sizeof snd);
    {
        int actual = 0;
        socklen_arg_t len = sizeof actual;
        if (getsockopt(s, SOL_SOCKET, SO_SNDBUF,
                       reinterpret_cast<char*>(&actual), &len) == 0) {
#ifndef _WIN32
            actual /= 2;      // Linux reports double what it will actually use
#endif
            sendBufferBytes_ = actual;
        }
    }

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(ep.port);
    dst.sin_addr = group;
    dstAddr_.resize(sizeof dst);
    std::memcpy(dstAddr_.data(), &dst, sizeof dst);

    sock_ = toHandle(s);
    lastError_.clear();
    sent_ = errors_ = 0;
    return true;
}

void MulticastSender::close() {
    if (sock_ != kInvalid) {
        closeSocket(asSocket(sock_));
        sock_ = kInvalid;
    }
    dstAddr_.clear();
    segmentBytes_ = 0;
}

int MulticastSender::send(std::span<const std::uint8_t> datagram) {
    if (sock_ == kInvalid || dstAddr_.empty()) return -1;
    const auto n = sendto(
        asSocket(sock_),
        reinterpret_cast<const char*>(datagram.data()),
        int(datagram.size()), 0,
        reinterpret_cast<const sockaddr*>(dstAddr_.data()),
        socklen_arg_t(dstAddr_.size()));
    if (n < 0) {
        ++errors_;
        lastError_ = "sendto: " + socketErrorText(socketError());
        return -1;
    }
    ++sent_;
    return int(n);
}

// UDP segmentation offload.
//
// One sendto() hands the stack a buffer of many fixed-size datagrams and it
// splits them, which is what makes ST 2022-6 packet rates reachable from a
// single thread. Windows spells it UDP_SEND_MSG_SIZE and Linux spells it
// UDP_SEGMENT (generic segmentation offload, kernel 4.18+); both are set once on
// the socket and then apply to every send, so the two are the same shape.
//
// Either can refuse -- an old kernel, a Windows build without the option, a NIC
// path that will not take it -- so the result is probed rather than assumed and
// sendMany() falls back to a loop of send().
bool MulticastSender::enableSegmentation(int segmentBytes) {
    segmentBytes_ = 0;
    if (sock_ == kInvalid || segmentBytes <= 0) return false;

#ifdef _WIN32
    const DWORD seg = DWORD(segmentBytes);
    const int level = IPPROTO_UDP, name = UDP_SEND_MSG_SIZE;
#else
    const int seg = segmentBytes;
    const int level = IPPROTO_UDP, name = UDP_SEGMENT;
#endif
    if (setsockopt(asSocket(sock_), level, name, sockoptPtr(&seg), sizeof seg) != 0) {
        lastError_ = "segmentation offload unsupported: " +
                     socketErrorText(socketError());
        return false;
    }
    segmentBytes_ = segmentBytes;
    return true;
}

int MulticastSender::sendMany(const std::uint8_t* buffer, int segmentBytes, int count) {
    if (sock_ == kInvalid || count <= 0) return -1;

    if (segmentBytes_ == segmentBytes) {
        const int total = segmentBytes * count;
        const auto n = sendto(
            asSocket(sock_), reinterpret_cast<const char*>(buffer), total, 0,
            reinterpret_cast<const sockaddr*>(dstAddr_.data()),
            socklen_arg_t(dstAddr_.size()));
        if (n < 0) {
            // A kernel that accepted the option can still refuse the send -- a
            // path with no offload support answers EIO or EINVAL. Fall back for
            // the rest of the session rather than failing the frame: a
            // gap on the wire reads to a receiver as a fault, and the loop below
            // still delivers every datagram, just with more syscalls.
            segmentBytes_ = 0;
            lastError_ = "segmented send refused, falling back to one send per "
                         "datagram: " + socketErrorText(socketError());
        } else {
            sent_ += std::uint64_t(count);
            return count;
        }
    }

    // Fallback: one syscall per datagram.
    int done = 0;
    for (int i = 0; i < count; ++i) {
        if (send({buffer + std::size_t(i) * segmentBytes, std::size_t(segmentBytes)}) < 0)
            break;
        ++done;
    }
    return done;
}

std::string MulticastSender::bufferShortfall(int asked) const {
    // Linux clamps SO_SNDBUF to net.core.wmem_max without reporting an error,
    // and the stock 212 kB is a fifth of what a burst here needs. The clamp does
    // not stop the replay -- the pacer's bursts are small -- but it does show up
    // as spacing jitter under load, and a silent clamp is exactly the kind of
    // thing that gets blamed on the application. So it is read back and said out
    // loud, once, rather than discovered with a packet capture.
    if (sendBufferBytes_ <= 0 || sendBufferBytes_ >= asked) return {};
    return "send buffer is " + std::to_string(sendBufferBytes_ / 1024) +
           " kB, asked for " + std::to_string(asked / 1024) +
           " kB (raise net.core.wmem_max)";
}

// ---------------------------------------------------------------------------

MulticastReceiver::~MulticastReceiver() { close(); }

bool MulticastReceiver::open(const MulticastEndpoint& ep, int recvBufferBytes) {
    close();

    in_addr group{};
    if (inet_pton(AF_INET, ep.group.c_str(), &group) != 1) {
        lastError_ = "invalid multicast group '" + ep.group + "'";
        return false;
    }

    socket_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kInvalidSocket) {
        lastError_ = "socket: " + socketErrorText(socketError());
        return false;
    }

#ifdef _WIN32
    // Disable SIO_UDP_CONNRESET.
    //
    // Windows puts a UDP socket into a permanent error state if any datagram it
    // sent elicits an ICMP Port Unreachable: every later recv() then fails with
    // WSAECONNRESET forever. On a receive-only multicast socket this is pure
    // sabotage -- the stream stops dead while the IGMP membership stays valid,
    // so the switch and bridge both still show us subscribed and everything
    // upstream looks perfect. Recovers only on socket recreate.
    //
    // Linux has no equivalent misbehaviour: an ICMP error on an unconnected UDP
    // socket is not reported to the application at all.
    {
        BOOL behaviour = FALSE;
        DWORD returned = 0;
        WSAIoctl(s, SIO_UDP_CONNRESET, &behaviour, sizeof behaviour,
                 nullptr, 0, &returned, nullptr, nullptr);
    }
#endif

    // Share the group with other listeners on this machine.
    const int reuse = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, sockoptPtr(&reuse), sizeof reuse);

    // Bind to the port on any address; the group membership does the filtering.
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(ep.port);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, reinterpret_cast<const sockaddr*>(&local), sizeof local) != 0) {
        lastError_ = "bind: " + socketErrorText(socketError());
        closeSocket(s);
        return false;
    }

    // A large receive buffer is essential: 1.485 Gb/s leaves very little time
    // to drain, and a short stall becomes packet loss the -7 merge then has to
    // paper over.
    const int rcv = recvBufferBytes;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, sockoptPtr(&rcv), sizeof rcv);

    ip_mreq mreq{};
    mreq.imr_multiaddr = group;
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (!ep.interfaceIp.empty()) {
        if (inet_pton(AF_INET, ep.interfaceIp.c_str(), &mreq.imr_interface) != 1) {
            lastError_ = "invalid interface address '" + ep.interfaceIp + "'";
            closeSocket(s);
            return false;
        }
    }
    if (setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   sockoptPtr(&mreq), sizeof mreq) != 0) {
        lastError_ = "IP_ADD_MEMBERSHIP: " + socketErrorText(socketError());
        closeSocket(s);
        return false;
    }

    mreq_.resize(sizeof mreq);
    std::memcpy(mreq_.data(), &mreq, sizeof mreq);
    joined_ = true;
    sock_ = toHandle(s);
    lastError_.clear();
    received_ = 0;
    return true;
}

void MulticastReceiver::close() {
    if (sock_ != kInvalid) {
        if (joined_ && mreq_.size() == sizeof(ip_mreq))
            setsockopt(asSocket(sock_), IPPROTO_IP, IP_DROP_MEMBERSHIP,
                       sockoptPtr(mreq_.data()), socklen_arg_t(mreq_.size()));
        closeSocket(asSocket(sock_));
        sock_ = kInvalid;
    }
    joined_ = false;
    mreq_.clear();
}

bool MulticastReceiver::rejoin() {
    if (sock_ == kInvalid || mreq_.size() != sizeof(ip_mreq)) return false;

    setsockopt(asSocket(sock_), IPPROTO_IP, IP_DROP_MEMBERSHIP,
               sockoptPtr(mreq_.data()), socklen_arg_t(mreq_.size()));
    if (setsockopt(asSocket(sock_), IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   sockoptPtr(mreq_.data()), socklen_arg_t(mreq_.size())) != 0) {
        lastError_ = "re-join failed: " + socketErrorText(socketError());
        joined_ = false;
        return false;
    }
    joined_ = true;
    ++rejoins_;
    return true;
}

int MulticastReceiver::receive(std::span<std::uint8_t> buffer, int timeoutMs) {
    if (sock_ == kInvalid) return -1;

    if (timeoutMs != timeoutMs_) {
        const RecvTimeout to(timeoutMs);
        setsockopt(asSocket(sock_), SOL_SOCKET, SO_RCVTIMEO, to.data(), to.size());
        timeoutMs_ = timeoutMs;
    }

    const auto n = recv(asSocket(sock_),
                        reinterpret_cast<char*>(buffer.data()),
                        int(buffer.size()), 0);
    if (n < 0) {
        const int e = socketError();
        if (timedOut(e)) return 0;
        lastError_ = "recv: " + socketErrorText(e);
        return -1;
    }
    ++received_;
    return int(n);
}

int MulticastReceiver::receiveBatch(std::uint8_t* buffer, std::size_t stride,
                                    int* lengths, int maxDatagrams, int timeoutMs) {
    if (sock_ == kInvalid || maxDatagrams <= 0) return -1;

    // First read blocks up to the timeout; the rest are drained non-blocking so
    // a burst is collected in one go.
    int count = 0;
    const int n0 = receive({buffer, stride}, timeoutMs);
    if (n0 < 0) return -1;
    if (n0 == 0) return 0;
    lengths[count++] = n0;

    setNonBlocking(asSocket(sock_), true);
    while (count < maxDatagrams) {
        const auto n = recv(asSocket(sock_),
                            reinterpret_cast<char*>(buffer + std::size_t(count) * stride),
                            int(stride), 0);
        if (n <= 0) break;
        lengths[count++] = int(n);
        ++received_;
    }
    setNonBlocking(asSocket(sock_), false);

    return count;
}

}  // namespace pcapreplay
