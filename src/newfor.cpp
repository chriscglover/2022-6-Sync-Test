#include "newfor.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace testsignal {

namespace {

constexpr std::uint8_t kConnect = 0x0E;
constexpr std::uint8_t kBuild = 0x0F;
constexpr std::uint8_t kReveal = 0x10;
constexpr std::uint8_t kClear = 0x18;

constexpr std::size_t kRowBytes = 2 + 40;

std::string pageText(int page) {
    char b[8];
    std::snprintf(b, sizeof b, "%X", page);
    return b;
}

// The visible text of a row, control codes as spaces, trimmed.
std::string rowText(std::span<const std::uint8_t> cells) {
    std::string s;
    for (std::uint8_t c : cells) {
        const std::uint8_t v = c & 0x7F;
        s.push_back(v >= 0x20 && v < 0x7F ? char(v) : ' ');
    }
    const auto first = s.find_first_not_of(' ');
    if (first == std::string::npos) return {};
    return s.substr(first, s.find_last_not_of(' ') - first + 1);
}

}  // namespace

NewforParser::NewforParser(TeletextPager& pager, Log log) : pager_(pager), log_(std::move(log)) {}

std::size_t NewforParser::commandLength() const {
    switch (buf_[0] & 0x7F) {
        case kConnect: return 5;
        case kReveal:
        case kClear: return 1;
        case kBuild: {
            if (buf_.size() < 2) return 0;
            const int ctl = hamming84Decode(buf_[1]);
            const int rows = ctl < 0 ? 0 : (ctl & 0x07);
            return 2 + std::size_t(rows) * kRowBytes;
        }
        default: return 1;   // unknown: skip it
    }
}

void NewforParser::feed(std::span<const std::uint8_t> bytes) {
    buf_.insert(buf_.end(), bytes.begin(), bytes.end());
    while (!buf_.empty()) {
        const std::size_t need = commandLength();
        if (need == 0 || need > buf_.size()) break;
        execute(need);
        buf_.erase(buf_.begin(), buf_.begin() + std::ptrdiff_t(need));
    }
}

void NewforParser::execute(std::size_t /*length*/) {
    const std::uint8_t op = buf_[0] & 0x7F;
    auto say = [&](const std::string& s) { if (log_) log_(s); };

    switch (op) {
        case kConnect: {
            const int mag = hamming84Decode(buf_[2]);
            const int tens = hamming84Decode(buf_[3]);
            const int units = hamming84Decode(buf_[4]);
            ++commands_;
            if (mag < 0 || tens < 0 || units < 0) {
                say("CONNECT with an uncorrectable page number, ignored");
                return;
            }
            if (mag == 9 && tens == 9 && units == 9) {
                pager_.disconnect();
                disconnected_ = true;
                say("DISCONNECT");
                return;
            }
            // NEWFOR sends the magazine as its digit, 1 to 8 (page 888 is D0 D0 D0).
            // FAB Subtitler follows CONNECT 888 with CONNECT 000 at the start of
            // every session; a real inserter keeps its page, so this does too.
            // Magazine 0 is not 8 here: that is teletext packet addressing.
            if (mag == 0 || mag > 8) {
                say("CONNECT page " + std::to_string(mag) + std::to_string(tens) + std::to_string(units) +
                    " is not a page, keeping " + pageText(pager_.page()));
                return;
            }
            const int page = (mag << 8) | (tens << 4) | units;
            pager_.connect(page);
            say("CONNECT page " + pageText(page));
            return;
        }
        case kBuild: {
            const int ctl = hamming84Decode(buf_[1]);
            ++commands_;
            if (ctl < 0) {
                say("BUILD with an uncorrectable control byte, ignored");
                return;
            }
            const bool clearFirst = (ctl & 0x08) != 0;
            const int rows = ctl & 0x07;
            std::string shown;
            for (int i = 0; i < rows; ++i) {
                const std::uint8_t* r = buf_.data() + 2 + std::size_t(i) * kRowBytes;
                const int hi = hamming84Decode(r[0]);
                const int lo = hamming84Decode(r[1]);
                if (hi < 0 || lo < 0) {
                    say("BUILD row with an uncorrectable row number, skipped");
                    continue;
                }
                const int row = (hi << 4) | lo;
                pager_.build(clearFirst && i == 0, row, std::span<const std::uint8_t, 40>(r + 2, 40));
                shown += (shown.empty() ? "" : " | ") + std::string("r") + std::to_string(row) + " \"" +
                         rowText({r + 2, 40}) + "\"";
            }
            if (rows == 0 && clearFirst) {
                // Clear-before-display with no rows: row 0 is never stored.
                static const std::array<std::uint8_t, 40> kNone{};
                pager_.build(true, 0, kNone);
            }
            say(std::string("BUILD") + (clearFirst ? " (clear)" : "") + " " + shown);
            return;
        }
        case kReveal:
            ++commands_;
            pager_.reveal();
            say("REVEAL");
            return;
        case kClear:
            ++commands_;
            pager_.clear();
            say("CLEAR");
            return;
        default: {
            ++unknown_;
            char b[64];
            std::snprintf(b, sizeof b, "unknown byte 0x%02X, skipped", buf_[0]);
            say(b);
            return;
        }
    }
}

// ---- server ------------------------------------------------------------------

NewforServer::~NewforServer() { stop(); }

bool NewforServer::start(const std::string& bindAddress, int port, bool verbose, std::string& error) {
    verbose_ = verbose;
    listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
        error = std::string("NEWFOR socket: ") + std::strerror(errno);
        return false;
    }
    const int one = 1;
    ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(std::uint16_t(port));
    if (bindAddress.empty()) {
        a.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (::inet_pton(AF_INET, bindAddress.c_str(), &a.sin_addr) != 1) {
        error = "NEWFOR bind address '" + bindAddress + "' is not an IPv4 address";
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }
    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(listenFd_, 4) != 0) {
        error = "NEWFOR listen on " + (bindAddress.empty() ? std::string("0.0.0.0") : bindAddress) + ":" +
                std::to_string(port) + ": " + std::strerror(errno);
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(mutex_);
        status_.listening = true;
    }
    stopping_ = false;
    thread_ = std::thread([this] { run(); });
    return true;
}

void NewforServer::stop() {
    stopping_ = true;
    if (thread_.joinable()) thread_.join();
    if (listenFd_ >= 0) ::close(listenFd_);
    listenFd_ = -1;
    std::lock_guard<std::mutex> lk(mutex_);
    status_.listening = false;
}

NewforStatus NewforServer::status() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return status_;
}

void NewforServer::run() {
    auto log = [this](const std::string& s) {
        if (verbose_) {
            std::printf("[newfor] %s\n", s.c_str());
            std::fflush(stdout);
        }
    };

    while (!stopping_) {
        pollfd p{listenFd_, POLLIN, 0};
        if (::poll(&p, 1, 200) <= 0) continue;
        sockaddr_in peer{};
        socklen_t len = sizeof peer;
        const int fd = ::accept(listenFd_, reinterpret_cast<sockaddr*>(&peer), &len);
        if (fd < 0) continue;
        const int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);

        char ip[INET_ADDRSTRLEN] = "?";
        ::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);
        const std::string who = std::string(ip) + ":" + std::to_string(ntohs(peer.sin_port));
        {
            std::lock_guard<std::mutex> lk(mutex_);
            status_.peer = who;
            ++status_.sessions;
        }
        log("session from " + who);

        NewforParser parser(pager_, log);
        std::uint8_t buf[4096];
        while (!stopping_ && !parser.disconnected()) {
            pollfd c{fd, POLLIN, 0};
            if (::poll(&c, 1, 200) <= 0) continue;
            const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
            if (n <= 0) break;
            const std::uint64_t before = parser.commands();
            parser.feed({buf, std::size_t(n)});
            std::lock_guard<std::mutex> lk(mutex_);
            status_.commands += parser.commands() - before;
        }
        ::close(fd);
        // However the session ended, its caption must not stay on air.
        if (!parser.disconnected()) pager_.disconnect();
        log("session from " + who + " closed");
        std::lock_guard<std::mutex> lk(mutex_);
        status_.peer.clear();
    }
}

}  // namespace testsignal
