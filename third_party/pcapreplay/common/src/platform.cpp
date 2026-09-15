#include "pcapreplay/platform.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#ifndef _WIN32
#include <csignal>
#endif

namespace pcapreplay {
namespace {
std::atomic<int> g_refs{0};
}

#ifdef _WIN32

std::string socketErrorText(int code) {
    char* msg = nullptr;
    const DWORD n = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, DWORD(code), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<char*>(&msg), 0, nullptr);
    std::string s;
    if (n && msg) {
        s.assign(msg, n);
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
    } else {
        s = "error " + std::to_string(code);
    }
    if (msg) LocalFree(msg);
    return s + " (" + std::to_string(code) + ")";
}

void ignoreSigPipe() {}

SocketScope::SocketScope() {
    WSADATA d{};
    ok_ = WSAStartup(MAKEWORD(2, 2), &d) == 0;
    if (ok_) g_refs.fetch_add(1);
}

SocketScope::~SocketScope() {
    if (ok_ && g_refs.fetch_sub(1) == 1) WSACleanup();
}

#else   // POSIX

std::string socketErrorText(int code) {
    // strerror_r has two incompatible signatures depending on _GNU_SOURCE, and
    // which one is in scope is not something to leave to the build. strerror is
    // not thread-safe in principle, but glibc's returns a pointer to immutable
    // per-errno text for every code that has one, and the fallback below covers
    // the case where it does not.
    char buf[256] = {};
    const char* msg = std::strerror(code);
    if (msg && *msg) std::snprintf(buf, sizeof buf, "%s", msg);
    else             std::snprintf(buf, sizeof buf, "error %d", code);
    return std::string(buf) + " (" + std::to_string(code) + ")";
}

// A peer that closes a connection while we are writing to it raises SIGPIPE,
// whose default action is to terminate the process. The HTTP server writes to
// sockets a controller may abandon at any moment, so this is not a theoretical
// concern -- it is the ordinary case of a browser navigating away mid-response.
// send() then returns EPIPE and is handled where it is checked.
void ignoreSigPipe() {
    std::signal(SIGPIPE, SIG_IGN);
}

SocketScope::SocketScope() {
    ok_ = true;
    if (g_refs.fetch_add(1) == 0) ignoreSigPipe();
}

SocketScope::~SocketScope() { g_refs.fetch_sub(1); }

#endif

}  // namespace pcapreplay
