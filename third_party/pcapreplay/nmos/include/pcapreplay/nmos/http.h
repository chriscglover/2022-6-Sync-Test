// A small HTTP/1.1 server and client, enough to be an NMOS node.
//
// This is not the loopback StatsServer in common/: an NMOS node has to be
// reachable by the registry and by controllers, so this one binds a real
// interface, routes by method and path, and speaks the verbs IS-05 needs --
// GET, PATCH, OPTIONS and HEAD. It also has to emit CORS headers on every
// response, including preflight, because the NMOS web controllers are browser
// applications and will not talk to a node that omits them.
//
// The client half exists for registration: IS-04 registration is the node
// POSTing itself to the registry and then heartbeating, which is an HTTP client
// job with no server involvement at all.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pcapreplay::nmos {

struct HttpRequest {
    std::string method;
    std::string path;                              // decoded, no query
    std::string query;
    std::string body;
    std::map<std::string, std::string> headers;    // keys lowercased
    std::map<std::string, std::string> params;     // from :name pattern segments
    std::string peer;                              // dotted quad, for logging

    std::string header(const std::string& k) const {
        const auto it = headers.find(k);
        return it == headers.end() ? std::string() : it->second;
    }
};

struct HttpResponse {
    int         status = 200;
    std::string contentType = "application/json";
    std::string body;
    std::vector<std::pair<std::string, std::string>> extraHeaders;

    void json(const std::string& text, int code = 200) {
        status = code;
        contentType = "application/json";
        body = text;
    }
    void text(const std::string& t, int code = 200) {
        status = code;
        contentType = "text/plain; charset=utf-8";
        body = t;
    }
};

using HttpHandler = std::function<void(const HttpRequest&, HttpResponse&)>;

class HttpServer {
public:
    HttpServer() = default;
    ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Pattern segments beginning with ':' capture into request.params, e.g.
    //   "/x-nmos/connection/v1.1/single/senders/:id/staged"
    // A pattern ending in "/*" matches any deeper path.
    void route(const std::string& method, const std::string& pattern, HttpHandler h);

    // bindIp empty or "0.0.0.0" listens on every interface. port 0 takes an
    // ephemeral one, which port() then reports -- useful because the NMOS
    // advertisement carries whatever we actually got.
    bool start(const std::string& bindIp, std::uint16_t port);
    void stop();

    bool running() const { return running_.load(std::memory_order_relaxed); }
    std::uint16_t port() const { return port_; }
    const std::string& error() const { return error_; }

    // Every request served, for a GUI activity counter.
    std::uint64_t requestsServed() const { return served_.load(std::memory_order_relaxed); }
    std::string   lastRequestLine() const;

private:
    struct Route {
        std::string method;
        std::vector<std::string> segments;
        bool wildcard = false;
        HttpHandler handler;
    };

    void accept();
    void serve(std::uintptr_t sock, std::string peer);
    bool dispatch(HttpRequest& req, HttpResponse& res);

    std::vector<Route>       routes_;
    std::uintptr_t           listener_ = ~std::uintptr_t(0);
    std::uint16_t            port_ = 0;
    std::string              error_;
    std::atomic<bool>        running_{false};
    std::atomic<std::uint64_t> served_{0};
    std::thread              acceptThread_;
    std::vector<std::thread> workers_;

    mutable std::mutex logMutex_;
    std::string        lastLine_;

    // Accepted connections waiting for a worker.
    std::mutex              queueMutex_;
    std::condition_variable queueCv_;
    std::vector<std::pair<std::uintptr_t, std::string>> queue_;
};

// ---------------------------------------------------------------------------

struct HttpResult {
    bool        ok = false;         // the exchange completed; check `status` too
    int         status = 0;
    std::string body;
    std::string error;
    std::map<std::string, std::string> headers;

    bool success() const { return ok && status >= 200 && status < 300; }
};

// Blocking HTTP/1.1 request with Connection: close. `host` may be a name or a
// dotted quad. Handles Content-Length, chunked, and close-terminated bodies.
HttpResult httpRequest(const std::string& host, std::uint16_t port,
                       const std::string& method, const std::string& target,
                       const std::string& body = {},
                       const std::string& contentType = "application/json",
                       int timeoutMs = 5000);

std::string urlDecode(const std::string& s);

// First port at or above `first` that nothing else is listening on, or 0 if the
// whole span is taken. Probed with the same SO_EXCLUSIVEADDRUSE the server binds
// with, so "free" here means the same thing it will mean there.
//
// This is inherently a hint: another process can take the port between the probe
// and the bind. The caller still has to handle start() failing -- it just will
// not fail for the ordinary reason, which is another copy of this app already
// running on the same machine.
std::uint16_t firstFreePort(const std::string& bindIp, std::uint16_t first,
                            int span = 20);

}  // namespace pcapreplay::nmos
