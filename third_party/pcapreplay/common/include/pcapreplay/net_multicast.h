// Multicast transmit and receive sockets.
//
// The interface is selected explicitly on both send and receive. On a
// multi-homed box the default route wins otherwise, and the failure is silent --
// traffic simply leaves via the wrong NIC. The reference Windows machine has a
// 10G Mellanox alongside a Hyper-V vSwitch and the Linux one has two datacentre
// NICs plus a docker bridge, so that is not a hypothetical on either.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "pcapreplay/platform.h"

namespace pcapreplay {

// Winsock startup/shutdown on Windows, a no-op shell on POSIX. Construct one
// per app. The old name is kept because it is what the call sites say and it is
// still accurate on the platform where it does something.
using WinsockScope = SocketScope;

struct MulticastEndpoint {
    std::string   group;          // e.g. "239.0.0.1"
    std::uint16_t port = 0;
    std::string   interfaceIp;    // local NIC address; empty = system default
};

// ---------------------------------------------------------------------------

class MulticastSender {
public:
    MulticastSender() = default;
    ~MulticastSender();
    MulticastSender(const MulticastSender&) = delete;
    MulticastSender& operator=(const MulticastSender&) = delete;

    // ttl 1 keeps traffic on the local segment; raise to cross a router.
    // loopback must be on for same-machine sender->receiver testing.
    bool open(const MulticastEndpoint& ep,
              int ttl = 8,
              bool loopback = true,
              int sendBufferBytes = 8 * 1024 * 1024);
    void close();
    bool isOpen() const { return sock_ != ~0ull; }

    // Returns bytes sent, or -1. Does not retry: at line rate a blocking retry
    // loop would wreck the pacing, so a failure is counted and reported.
    int send(std::span<const std::uint8_t> datagram);

    // UDP Segmentation Offload. One sendto() hands the stack a buffer of many
    // fixed-size datagrams and it splits them, which is the only way to reach
    // ST 2022-6 packet rates on Windows: a plain sendto() costs ~5.7 us, so
    // 1080p50 across two paths would need 539,700 syscalls/sec from one thread
    // against a measured ceiling of ~177,000.
    //
    // Returns false if the platform will not do it, in which case sendMany()
    // transparently falls back to a loop of send().
    bool enableSegmentation(int segmentBytes);
    bool segmentationEnabled() const { return segmentBytes_ > 0; }

    // Send `count` datagrams of exactly `segmentBytes` laid out back to back.
    // Returns datagrams accepted, or -1.
    int sendMany(const std::uint8_t* buffer, int segmentBytes, int count);

    // Largest count sendMany() will take in one call (64 KB / segment).
    static int maxSegments(int segmentBytes) {
        return segmentBytes > 0 ? 65507 / segmentBytes : 1;
    }

    const std::string& lastError() const { return lastError_; }
    std::uint64_t      datagramsSent() const { return sent_; }
    std::uint64_t      sendErrors()    const { return errors_; }

    // Empty unless the kernel gave us a smaller send buffer than open() asked
    // for, in which case this says so in a form fit to print. Linux clamps
    // SO_SNDBUF to net.core.wmem_max silently, and a silent clamp shows up later
    // as spacing jitter that gets blamed on the application.
    std::string bufferShortfall(int asked = 8 * 1024 * 1024) const;

private:
    std::uintptr_t sock_ = ~0ull;
    std::vector<std::uint8_t> dstAddr_;   // sockaddr_in
    int            segmentBytes_ = 0;
    int            sendBufferBytes_ = 0;  // what the kernel actually granted
    std::string    lastError_;
    std::uint64_t  sent_ = 0;
    std::uint64_t  errors_ = 0;
};

// ---------------------------------------------------------------------------

class MulticastReceiver {
public:
    MulticastReceiver() = default;
    ~MulticastReceiver();
    MulticastReceiver(const MulticastReceiver&) = delete;
    MulticastReceiver& operator=(const MulticastReceiver&) = delete;

    bool open(const MulticastEndpoint& ep,
              int recvBufferBytes = 32 * 1024 * 1024);
    void close();
    bool isOpen() const { return sock_ != ~0ull; }

    // Blocking receive with a timeout. Returns bytes received, 0 on timeout,
    // -1 on error.
    //
    // Uses SO_RCVTIMEO rather than a select() per datagram. At 135k packets/sec
    // per path, a select() before every recv() doubles the syscall count for no
    // benefit -- it was measurably throttling the receiver.
    int receive(std::span<std::uint8_t> buffer, int timeoutMs = 100);

    // Drain up to `maxDatagrams` without blocking after the first. Returns how
    // many were read. Batching amortises both the syscall and the downstream
    // locking, which matters far more than per-call latency here.
    int receiveBatch(std::uint8_t* buffer, std::size_t stride,
                     int* lengths, int maxDatagrams, int timeoutMs = 100);

    // Leave and re-join the group, forcing a fresh IGMP membership report.
    //
    // Snooping switches and Linux bridges age memberships out and prune the
    // port; the stream then stops dead until something re-joins. Re-joining
    // recovers automatically, but it is a workaround for a network that is not
    // refreshing memberships -- the rejoin counter is surfaced so the
    // underlying problem stays visible rather than being papered over.
    bool rejoin();

    const std::string& lastError() const { return lastError_; }
    std::uint64_t      datagramsReceived() const { return received_; }
    std::uint64_t      rejoins() const { return rejoins_; }

private:
    std::uintptr_t sock_ = ~0ull;
    int            timeoutMs_ = -1;
    bool           joined_ = false;
    std::uint64_t  rejoins_ = 0;
    std::vector<std::uint8_t> mreq_;      // ip_mreq, for leaving cleanly
    std::string    lastError_;
    std::uint64_t  received_ = 0;
};

// Kept as the name the call sites use; socketErrorText() is the portable one.
inline std::string winsockErrorText(int code) { return socketErrorText(code); }

}  // namespace pcapreplay
