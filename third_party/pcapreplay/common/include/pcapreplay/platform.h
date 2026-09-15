// The thin layer between Winsock and Berkeley sockets.
//
// The two APIs are close enough that the rest of the codebase does not need to
// know which one it is on: what differs is the handle type, how an error is
// read back, how a socket is closed, and how it is put into non-blocking mode.
// Those four are wrapped here, and everything else -- setsockopt names, the
// sockaddr layouts, the multicast options -- is common to both.
//
// Deliberately not a socket class. The existing code is written against the C
// API and reads the way network code in this field is normally written; wrapping
// it in objects would obscure rather than clarify, and the port is meant to be
// reviewable against the Windows original.
#pragma once

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#else   // POSIX

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#endif

#include <cstdint>
#include <string>

namespace pcapreplay {

#ifdef _WIN32

using socket_t = SOCKET;
inline constexpr socket_t kInvalidSocket = INVALID_SOCKET;

inline int  socketError()            { return WSAGetLastError(); }
inline void closeSocket(socket_t s)  { closesocket(s); }
inline bool wouldBlock(int e)        { return e == WSAEWOULDBLOCK; }
inline bool timedOut(int e)          { return e == WSAETIMEDOUT; }

inline void setNonBlocking(socket_t s, bool on) {
    u_long v = on ? 1 : 0;
    ioctlsocket(s, FIONBIO, &v);
}

// setsockopt's value argument is `const char*` on Winsock and `const void*` on
// POSIX; this keeps the call sites free of casts.
using sockopt_t = const char*;
inline sockopt_t sockoptPtr(const void* p) { return static_cast<sockopt_t>(p); }
using socklen_arg_t = int;

// Receive timeouts are a DWORD of milliseconds on Windows and a timeval on
// POSIX, so the option value itself has to be built per platform.
struct RecvTimeout {
    DWORD ms;
    explicit RecvTimeout(int milliseconds) : ms(DWORD(milliseconds)) {}
    const char* data() const { return reinterpret_cast<const char*>(&ms); }
    int size() const { return int(sizeof ms); }
};

// Windows has a real "one process only" bind. POSIX has no equivalent -- see
// the note in stats_server.cpp -- so this is where the difference is stated
// once rather than at each call site.
inline void setExclusiveBind(socket_t s) {
    const DWORD exclusive = 1;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
               reinterpret_cast<const char*>(&exclusive), sizeof exclusive);
}

// Winsock spells shutdown()'s argument SD_BOTH and POSIX spells it SHUT_RDWR;
// the values are the same. The POSIX name is used at the call sites.
#ifndef SHUT_RDWR
#define SHUT_RDWR SD_BOTH
#endif

#else   // POSIX

using socket_t = int;
inline constexpr socket_t kInvalidSocket = -1;

inline int  socketError()            { return errno; }
inline void closeSocket(socket_t s)  { ::close(s); }
inline bool wouldBlock(int e)        { return e == EWOULDBLOCK || e == EAGAIN; }
inline bool timedOut(int e)          { return e == EWOULDBLOCK || e == EAGAIN; }

inline void setNonBlocking(socket_t s, bool on) {
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return;
    flags = on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    fcntl(s, F_SETFL, flags);
}

using sockopt_t = const void*;
inline sockopt_t sockoptPtr(const void* p) { return p; }
using socklen_arg_t = socklen_t;

struct RecvTimeout {
    timeval tv;
    explicit RecvTimeout(int milliseconds) {
        tv.tv_sec  = milliseconds / 1000;
        tv.tv_usec = (milliseconds % 1000) * 1000;
    }
    const void* data() const { return &tv; }
    socklen_t size() const { return socklen_t(sizeof tv); }
};

// No POSIX equivalent of SO_EXCLUSIVEADDRUSE exists, and none is needed: a
// plain bind() without SO_REUSEADDR already refuses a port another process is
// listening on, which is the property the Windows code was reaching for. The
// hijack SO_EXCLUSIVEADDRUSE defends against is a Windows-specific consequence
// of SO_REUSEADDR being far more permissive there.
inline void setExclusiveBind(socket_t) {}

#endif

// A few headers store a socket as a plain uintptr_t so they need not pull the
// platform's network headers into everything that includes them. These are the
// only two conversions, and going through intptr_t is what makes the POSIX -1
// and the Windows INVALID_SOCKET land on the same all-ones value.
inline constexpr std::uintptr_t kInvalidHandle = ~std::uintptr_t(0);
inline std::uintptr_t toHandle(socket_t s) {
    return std::uintptr_t(std::intptr_t(s));
}
inline socket_t fromHandle(std::uintptr_t h) {
    return socket_t(std::intptr_t(h));
}

// Human-readable text for a socket error code, with the number in brackets.
std::string socketErrorText(int code);

// Winsock needs starting and stopping; on POSIX this is an empty shell kept so
// callers do not have to bracket it in #ifdef. Reference counted, one per app.
class SocketScope {
public:
    SocketScope();
    ~SocketScope();
    SocketScope(const SocketScope&) = delete;
    SocketScope& operator=(const SocketScope&) = delete;
    bool ok() const { return ok_; }
private:
    bool ok_ = false;
};

// SIGPIPE would otherwise kill the process when a peer closes a connection
// mid-write -- the HTTP server does that routinely. Called once from
// SocketScope; harmless on Windows.
void ignoreSigPipe();

}  // namespace pcapreplay
