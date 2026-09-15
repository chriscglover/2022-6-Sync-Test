#include "pcapreplay/nmos/nmos_node.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>

#include "pcapreplay/nmos/http.h"
#include "pcapreplay/nmos/json.h"
#include "pcapreplay/nmos/mdns.h"
#include "pcapreplay/nmos/uuid.h"

namespace pcapreplay::nmos {
namespace {

using Clock = std::chrono::steady_clock;

// The port an instance gets unless something is already on it. Instances that
// land here keep the identity they had before per-instance seeding existed.
constexpr std::uint16_t kDefaultNodePort = 3210;

// The short format token out of the engine's description -- "1080i25" from
// "1080i25 (1080i50)  1920x1080  1.485 Gb/s". Labels want the token; the full
// text stays in the description, where there is room for it.
std::string shortFormat(const std::string& formatText) {
    const std::size_t sp = formatText.find(' ');
    return sp == std::string::npos ? formatText : formatText.substr(0, sp);
}

std::string joinUrl(const std::string& host, std::uint16_t port,
                    const std::string& path) {
    return "http://" + host + ":" + std::to_string(port) + path;
}

Json listing(std::initializer_list<const char*> entries) {
    Json a = Json::array();
    for (const char* e : entries) a.push(Json(e));
    return a;
}

}  // namespace

bool SenderTransport::operator==(const SenderTransport& o) const {
    return active == o.active && redundant == o.redundant &&
           sourceIpA == o.sourceIpA && sourceIpB == o.sourceIpB &&
           destIpA == o.destIpA && destIpB == o.destIpB &&
           destPortA == o.destPortA && destPortB == o.destPortB &&
           sourcePortA == o.sourcePortA && sourcePortB == o.sourcePortB &&
           ttl == o.ttl && ssrc == o.ssrc && payloadType == o.payloadType &&
           formatText == o.formatText &&
           frameRateNum == o.frameRateNum && frameRateDen == o.frameRateDen;
}

// ---------------------------------------------------------------------------

namespace {

class BuiltinNode final : public NmosBackend {
public:
    ~BuiltinNode() override { stop(); }

    void setCallbacks(QueryFn query, ApplyFn apply) override {
        query_ = std::move(query);
        apply_ = std::move(apply);
    }

    bool start(const NmosConfig& cfg) override;
    void stop() override;
    NmosStatus status() const override;
    // The GUI's Start and Stop buttons reach IS-05 through here.
    //
    // Re-syncing master_enable is the point of it. IS-05 says a PATCH is a
    // partial update of `staged`, so anything the body leaves out keeps its
    // staged value -- which is right, and is what nmos-cpp does. It only works
    // if `staged` tracks reality, and here reality can move without IS-05
    // touching it: the GUI can start and stop the replay itself. Seeding these
    // once at startup, when the engine has never run, left staged master_enable
    // false for the life of the process. A controller that then PATCHed a new
    // destination_ip and activated it -- without naming master_enable, because
    // it was not changing it -- activated that stale false, and the node
    // answered a request to move the sender by stopping it.
    void notifyChanged() override {
        syncEnableFromEngine();
        bumpVersion();
        wake();
    }

private:
    // ---- identity ---------------------------------------------------------
    void mintIds();
    void bumpVersion() {
        std::lock_guard<std::mutex> lk(mutex_);
        version_ = taiVersion();
        dirty_ = true;
    }
    std::string version() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return version_;
    }

    SenderTransport transport() const {
        return query_ ? query_() : SenderTransport{};
    }

    // Point staged and active master_enable at whether the engine is actually
    // transmitting. Call with no lock held: transport() calls back into the app.
    void syncEnableFromEngine() {
        const bool live = transport().active;
        std::lock_guard<std::mutex> lk(is05Mutex_);
        masterEnable_ = live;
        // Only follow for the staged copy if nothing is staged and waiting. A
        // controller that has PATCHed master_enable but not yet activated it is
        // entitled to have that survive.
        if (!stagedValid_) stagedMasterEnable_ = live;
    }

    // What a controller shows in its list, and the only thing distinguishing one
    // of these from another on screen -- so it has to carry both axes along which
    // instances multiply. The port tells two copies on one machine apart; the
    // host tells copies on different machines apart, which the port alone cannot
    // because each machine hands out 3210 first.
    //
    // The UUIDs were never at risk across hosts -- the machine name has always
    // been in the seed -- but a registry full of identically named senders is its
    // own kind of unusable.
    //
    // host:port rather than "host port": it is the familiar idiom for an
    // endpoint, and it reads as one token rather than two loose words.
    std::string instanceLabel() const {
        return cfg_.label + " " + hostLabel_ + ":" + std::to_string(nodePort_);
    }
    // ... and for the stream resources, what is loaded as well, since that is
    // what someone is actually picking between. Safe to put in a label: labels
    // may change over a resource's life, which is exactly why the format is here
    // and not in the seed. See mintIds().
    std::string streamLabel(const SenderTransport& t) const {
        const std::string f = shortFormat(t.formatText);
        return f.empty() ? instanceLabel() : instanceLabel() + " - " + f;
    }

    // The BCP-002-01 group hint: "<group name>:<role in group>". A controller
    // uses it to gather everything belonging to one physical thing under one
    // heading, so the group has to name the instance and nothing finer. The
    // loaded format is deliberately left out even though the label carries it:
    // opening a different capture would otherwise move the sender into a
    // brand new group, which is exactly the churn grouping exists to avoid.
    //
    // Host and port both appear for the same reason they are in the label --
    // the port separates two copies on one machine, the host separates copies
    // on different machines, and every machine hands out 3210 first. Two hosts
    // grouped as one "PCAP Replay-3210" would be a worse lie than no grouping.
    //
    // The colon is the delimiter, so it cannot appear inside either half:
    // instanceLabel()'s host:port is written host-port here, and a colon typed
    // into the Label box is folded the same way rather than being read as an
    // early end to the group name.
    std::string groupHint() const {
        std::string group = cfg_.label + " " + hostLabel_ + "-" +
                            std::to_string(nodePort_);
        for (char& c : group) if (c == ':') c = '-';
        // The role names the essence, not the leg count: an ST 2022-7 pair is
        // one sender with a redundant path, so toggling -7 must not rename it.
        return group + ":2022-6";
    }

    // ---- resources --------------------------------------------------------
    Json nodeResource() const;
    Json deviceResource() const;
    Json sourceResource() const;
    Json flowResource() const;
    Json senderResource() const;
    std::string sdp() const;

    Json transportParams(const SenderTransport& t) const;
    Json activeObject() const;
    Json stagedObject() const;
    Json constraints() const;

    // IS-05 state read under is05Mutex_. Everything that needs it goes through
    // here so the lock is never held across a call that takes mutex_ -- the two
    // are always taken mutex_ then is05Mutex_, never the other way round.
    void snapshotIs05(bool& masterEnable, std::string& receiverId) const {
        std::lock_guard<std::mutex> lk(is05Mutex_);
        masterEnable = masterEnable_;
        receiverId   = activeReceiverId_;
    }

    // ---- HTTP -------------------------------------------------------------
    void installRoutes();
    void handleStagedPatch(const HttpRequest& req, HttpResponse& res);

    // ---- registration -----------------------------------------------------
    void registrationLoop();
    bool discoverRegistry();
    bool registerAll();
    bool postResource(const char* type, const Json& data, std::string& err);
    bool heartbeat();
    void unregister();
    void wake() { cv_.notify_all(); }
    void setState(const std::string& s) {
        std::lock_guard<std::mutex> lk(mutex_);
        state_ = s;
    }
    void setLastActivation(const std::string& s) {
        std::lock_guard<std::mutex> lk(mutex_);
        lastActivation_ = s;
    }

    NmosConfig  cfg_;
    QueryFn     query_;
    ApplyFn     apply_;

    HttpServer  server_;
    MdnsBrowser browser_;
    MdnsAdvertiser advertiser_;

    std::thread             thread_;
    std::atomic<bool>       running_{false};
    std::atomic<bool>       stopping_{false};
    std::condition_variable cv_;

    mutable std::mutex mutex_;
    std::string nodeId_, deviceId_, sourceId_, flowId_, senderId_;
    std::string version_;
    std::string state_ = "idle";
    std::string error_, warning_;
    std::string registryHost_;
    std::uint16_t registryPort_ = 0;
    std::string   registryDiscovery_;    // how registryHost_ was arrived at
    std::string   registryServiceType_;  // the DNS-SD type it answered on
    bool          registered_ = false;
    bool          dirty_ = false;
    std::uint64_t heartbeats_ = 0, heartbeatFailures_ = 0;
    Clock::time_point lastHeartbeatOk_{};
    bool          everHeartbeat_ = false;
    std::string   lastActivation_;
    std::string   mdnsRejection_;
    std::string   hostLabel_;
    // The port actually bound, which is what the labels, the seed and the
    // published hrefs all have to agree on. Not necessarily cfg_.nodePort: a
    // second instance on the same machine gets the next free one.
    std::uint16_t nodePort_ = 0;
    std::uint16_t requestedPort_ = 0;

    // IS-05 state. `staged_` is what a controller has PATCHed but not yet
    // activated; on activation it is pushed into the engine and cleared.
    mutable std::mutex is05Mutex_;
    SenderTransport staged_;
    bool            stagedValid_ = false;
    bool            stagedMasterEnable_ = true;
    std::string     stagedReceiverId_;
    std::string     activeReceiverId_;
    bool            masterEnable_ = true;
};

// ---- identity ---------------------------------------------------------------

// Resource identity. Deterministic, so it survives a restart and a controller's
// existing route still points at something that exists.
//
// The node port is part of the seed once it is not the default, which is what
// makes a second instance on the same machine a genuinely different node rather
// than a duplicate of the first. Only once it is not the default: an instance on
// 3210 keeps exactly the UUIDs it had before this was introduced, so upgrading
// does not orphan a route that already works.
//
// The loaded format is deliberately NOT in the seed, although it is in the
// label. A UUID is an identity and has to outlive the thing it identifies
// changing -- folding the format in would mint a brand new node, device, source,
// flow and sender every time a different capture was loaded, and every route a
// controller had made would break. Labels are free to change; identities are not.
void BuiltinNode::mintIds() {
    hostLabel_ = localHostLabel();
    std::string seed = cfg_.seed.empty() ? (hostLabel_ + "/PCAP Replay") : cfg_.seed;
    if (cfg_.seed.empty() && nodePort_ != 0 && nodePort_ != kDefaultNodePort)
        seed += "/" + std::to_string(nodePort_);
    nodeId_   = resourceId(seed, "node");
    deviceId_ = resourceId(seed, "device");
    sourceId_ = resourceId(seed, "source");
    flowId_   = resourceId(seed, "flow");
    senderId_ = resourceId(seed, "sender");
    version_  = taiVersion();
}

// ---- resources ---------------------------------------------------------------

Json BuiltinNode::nodeResource() const {
    const SenderTransport t = transport();
    const std::string host = cfg_.nodeIp.empty() ? hostLabel_ : cfg_.nodeIp;
    const std::uint16_t port = server_.port();

    Json api = Json::object();
    Json versions = Json::array();
    versions.push(Json(cfg_.registrationVersion));
    api["versions"] = versions;
    Json endpoints = Json::array();
    Json ep = Json::object();
    ep["host"]     = Json(host);
    ep["port"]     = Json(std::int64_t(port));
    ep["protocol"] = Json("http");
    endpoints.push(ep);
    api["endpoints"] = endpoints;

    Json clocks = Json::array();
    Json clk = Json::object();
    clk["name"]     = Json("clk0");
    // Honest: the replay is paced by the machine's own clock, not locked to PTP.
    clk["ref_type"] = Json("internal");
    clocks.push(clk);

    // interface_bindings on the sender must name entries in this list.
    Json interfaces = Json::array();
    auto addIface = [&](const std::string& name, const std::string& mac) {
        if (name.empty()) return;
        for (const auto& e : interfaces.elements())
            if (e.at("name").asString() == name) return;
        Json i = Json::object();
        i["name"]       = Json(name);
        i["chassis_id"] = mac.empty() ? Json() : Json(mac);
        i["port_id"]    = mac.empty() ? Json() : Json(mac);
        interfaces.push(i);
    };
    addIface(t.interfaceNameA, t.macA);
    if (t.redundant) addIface(t.interfaceNameB, t.macB);
    if (interfaces.size() == 0) addIface("eth0", {});

    Json j = Json::object();
    j["id"]          = Json(nodeId_);
    j["version"]     = Json(version_);
    j["label"]       = Json(instanceLabel());
    j["description"] = Json(cfg_.description);
    j["tags"]        = Json::object();
    j["href"]        = Json(joinUrl(host, port, "/"));
    j["hostname"]    = Json(hostLabel_);
    j["api"]         = api;
    j["caps"]        = Json::object();
    j["services"]    = Json::array();
    j["clocks"]      = clocks;
    j["interfaces"]  = interfaces;
    return j;
}

Json BuiltinNode::deviceResource() const {
    const std::string host = cfg_.nodeIp.empty() ? hostLabel_ : cfg_.nodeIp;
    const std::uint16_t port = server_.port();

    Json controls = Json::array();
    Json c = Json::object();
    c["type"] = Json("urn:x-nmos:control:sr-ctrl/" + cfg_.connectionVersion);
    c["href"] = Json(joinUrl(host, port,
                             "/x-nmos/connection/" + cfg_.connectionVersion + "/"));
    c["authorization"] = Json(false);
    controls.push(c);

    Json senders = Json::array();
    senders.push(Json(senderId_));

    Json j = Json::object();
    j["id"]          = Json(deviceId_);
    j["version"]     = Json(version_);
    j["label"]       = Json(instanceLabel());
    j["description"] = Json(cfg_.description);
    j["tags"]        = Json::object();
    j["type"]        = Json("urn:x-nmos:device:generic");
    j["node_id"]     = Json(nodeId_);
    j["senders"]     = senders;
    j["receivers"]   = Json::array();
    j["controls"]    = controls;
    return j;
}

Json BuiltinNode::sourceResource() const {
    const SenderTransport t = transport();
    Json rate = Json::object();
    rate["numerator"]   = Json(std::int64_t(t.frameRateNum));
    rate["denominator"] = Json(std::int64_t(t.frameRateDen));

    Json j = Json::object();
    j["id"]          = Json(sourceId_);
    j["version"]     = Json(version_);
    j["label"]       = Json(streamLabel(t) + " source");
    j["description"] = Json(t.formatText.empty() ? cfg_.description : t.formatText);
    j["tags"]        = Json::object();
    j["caps"]        = Json::object();
    j["device_id"]   = Json(deviceId_);
    j["parents"]     = Json::array();
    j["clock_name"]  = Json("clk0");
    j["grain_rate"]  = rate;
    // ST 2022-6 carries a whole SDI signal -- video, embedded audio and all the
    // ancillary data together -- so it is a mux, not a video flow.
    j["format"]      = Json("urn:x-nmos:format:mux");
    return j;
}

Json BuiltinNode::flowResource() const {
    const SenderTransport t = transport();
    Json rate = Json::object();
    rate["numerator"]   = Json(std::int64_t(t.frameRateNum));
    rate["denominator"] = Json(std::int64_t(t.frameRateDen));

    Json j = Json::object();
    j["id"]          = Json(flowId_);
    j["version"]     = Json(version_);
    j["label"]       = Json(streamLabel(t) + " flow");
    j["description"] = Json(t.formatText.empty() ? cfg_.description : t.formatText);
    j["tags"]        = Json::object();
    j["source_id"]   = Json(sourceId_);
    j["device_id"]   = Json(deviceId_);
    j["parents"]     = Json::array();
    j["grain_rate"]  = rate;
    j["format"]      = Json("urn:x-nmos:format:mux");
    j["media_type"]  = Json("video/SMPTE2022-6");
    return j;
}

Json BuiltinNode::senderResource() const {
    const SenderTransport t = transport();
    const std::string host = cfg_.nodeIp.empty() ? hostLabel_ : cfg_.nodeIp;
    const std::uint16_t port = server_.port();

    Json bindings = Json::array();
    bindings.push(Json(t.interfaceNameA.empty() ? "eth0" : t.interfaceNameA));
    if (t.redundant)
        bindings.push(Json(t.interfaceNameB.empty() ? "eth0" : t.interfaceNameB));

    bool enabled = false;
    std::string receiverId;
    snapshotIs05(enabled, receiverId);

    Json sub = Json::object();
    sub["receiver_id"] = receiverId.empty() ? Json() : Json(receiverId);
    // Live transmit state, for the same reason as active.master_enable.
    sub["active"]      = Json(t.active);

    Json j = Json::object();
    j["id"]            = Json(senderId_);
    j["version"]       = Json(version_);
    // The one a controller routes from, so it carries both discriminators: which
    // instance, and what it is playing.
    j["label"]         = Json(streamLabel(t));
    j["description"]   = Json(t.formatText.empty() ? cfg_.description : t.formatText);
    // The group hint lives on the sender and nowhere else: BCP-002-01 tags
    // senders and receivers, which are what a controller lays out side by side,
    // not the source and flow behind them.
    Json hint = Json::array();
    hint.push(Json(groupHint()));
    Json tags = Json::object();
    tags["urn:x-nmos:tag:grouphint/v1.0"] = hint;
    j["tags"]          = tags;
    j["flow_id"]       = Json(flowId_);
    j["transport"]     = Json("urn:x-nmos:transport:rtp.mcast");
    j["device_id"]     = Json(deviceId_);
    j["manifest_href"] = Json(joinUrl(host, port,
        "/x-nmos/connection/" + cfg_.connectionVersion +
        "/single/senders/" + senderId_ + "/transportfile"));
    j["interface_bindings"] = bindings;
    j["subscription"]  = sub;
    j["caps"]          = Json::object();
    return j;
}

std::string BuiltinNode::sdp() const {
    const SenderTransport t = transport();
    std::string s;
    auto line = [&](const std::string& l) { s += l; s += "\r\n"; };

    // The origin should name a real address. Fall back to whatever the node API
    // is bound to before giving up and emitting the unspecified address, which
    // some receivers reject outright.
    std::string originIp = t.sourceIpA;
    if (originIp.empty()) originIp = cfg_.nodeIp;
    if (originIp.empty()) originIp = "0.0.0.0";
    // The SDP version has to advance whenever the description changes, and the
    // resource version is already doing exactly that job.
    std::string sdpVer = version_;
    for (char& c : sdpVer) if (c == ':') c = ' ';
    const std::string sessionId = std::to_string(
        std::hash<std::string>{}(senderId_) & 0x7FFFFFFFull);
    const std::string sessionVer = std::to_string(
        std::hash<std::string>{}(version_) & 0x7FFFFFFFull);

    line("v=0");
    line("o=- " + sessionId + " " + sessionVer + " IN IP4 " + originIp);
    line("s=" + streamLabel(t));
    if (!t.formatText.empty()) line("i=" + t.formatText);
    line("t=0 0");

    // RFC 7104 duplication grouping. Without this a receiver has no way to know
    // the two m= lines are copies of one another rather than two essences, which
    // is the whole of ST 2022-7 as far as SDP is concerned. The mid tokens are
    // opaque, but "primary"/"secondary" is what real ST 2022-6 kit emits and
    // matching it costs nothing.
    if (t.redundant) line("a=group:DUP primary secondary");

    auto leg = [&](const std::string& dest, std::uint16_t port,
                   const std::string& src, const std::string& mac,
                   const char* mid) {
        line("m=video " + std::to_string(port) + " RTP/AVP " +
             std::to_string(t.payloadType));
        line("c=IN IP4 " + dest + "/" + std::to_string(t.ttl));
        if (!src.empty())
            line("a=source-filter: incl IN IP4 " + dest + " " + src);
        line("a=rtpmap:" + std::to_string(t.payloadType) + " SMPTE2022-6/27000000");
        // Traffic shape only. The pacer places every datagram against an
        // absolute QPC schedule rather than bursting a frame at line rate, so
        // the wide profile is an honest description. No TROFF: that is an
        // offset from a PTP epoch and this sender has no PTP reference.
        line("a=fmtp:" + std::to_string(t.payloadType) + " TP=2110TPW;");
        // RFC 7273. Real kit here says ptp=IEEE1588-2008:traceable; this
        // machine is not locked to PTP, so it declares its own MAC as the
        // clock domain instead. Claiming traceable PTP we do not have would
        // make a receiver's timing decisions wrong rather than merely
        // conservative.
        if (!mac.empty()) line("a=ts-refclk:localmac=" + mac);
        line("a=mediaclk:direct=0");
        if (mid) line(std::string("a=mid:") + mid);
    };

    leg(t.destIpA, t.destPortA, t.sourceIpA, t.macA,
        t.redundant ? "primary" : nullptr);
    if (t.redundant) leg(t.destIpB, t.destPortB, t.sourceIpB, t.macB, "secondary");
    return s;
}

// ---- IS-05 objects -----------------------------------------------------------

Json BuiltinNode::transportParams(const SenderTransport& t) const {
    Json arr = Json::array();
    auto leg = [&](const std::string& src, const std::string& dst,
                   std::uint16_t sport, std::uint16_t dport) {
        Json p = Json::object();
        p["source_ip"]        = src.empty() ? Json() : Json(src);
        p["destination_ip"]   = Json(dst);
        p["source_port"]      = Json(std::int64_t(sport));
        p["destination_port"] = Json(std::int64_t(dport));
        p["rtp_enabled"]      = Json(t.active);
        arr.push(p);
    };
    leg(t.sourceIpA, t.destIpA, t.sourcePortA, t.destPortA);
    if (t.redundant) leg(t.sourceIpB, t.destIpB, t.sourcePortB, t.destPortB);
    return arr;
}

Json BuiltinNode::activeObject() const {
    const SenderTransport t = transport();
    bool enabled = false;
    std::string receiverId;
    snapshotIs05(enabled, receiverId);

    Json act = Json::object();
    act["mode"]            = Json();
    act["requested_time"]  = Json();
    act["activation_time"] = Json();

    Json j = Json::object();
    j["receiver_id"]      = receiverId.empty() ? Json() : Json(receiverId);
    // Whether the sender is actually transmitting, read live rather than from
    // the last thing a controller asked for. The two diverge whenever the GUI
    // starts or stops the replay, and a controller that believes a running
    // sender is disabled will not route it.
    (void)enabled;
    j["master_enable"]    = Json(t.active);
    j["activation"]       = act;
    j["transport_params"] = transportParams(t);
    return j;
}

Json BuiltinNode::stagedObject() const {
    // transport() calls back into the app, so read the staged copy out under
    // the lock and build the JSON outside it.
    SenderTransport t;
    bool haveStaged, enabled;
    std::string receiverId;
    {
        std::lock_guard<std::mutex> lk(is05Mutex_);
        haveStaged = stagedValid_;
        if (haveStaged) t = staged_;
        enabled    = stagedMasterEnable_;
        receiverId = stagedReceiverId_;
    }
    if (!haveStaged) t = transport();

    Json act = Json::object();
    act["mode"]            = Json();
    act["requested_time"]  = Json();
    act["activation_time"] = Json();

    Json j = Json::object();
    j["receiver_id"]      = receiverId.empty() ? Json() : Json(receiverId);
    j["master_enable"]    = Json(enabled);
    j["activation"]       = act;
    j["transport_params"] = transportParams(t);
    return j;
}

Json BuiltinNode::constraints() const {
    const SenderTransport t = transport();
    Json arr = Json::array();
    auto leg = [&](const std::string& src) {
        Json c = Json::object();
        Json srcEnum = Json::array();
        if (!src.empty()) srcEnum.push(Json(src));
        Json srcCon = Json::object();
        // The outgoing NIC is chosen in the GUI, so a controller may read it but
        // not change it -- an enum of one is how IS-05 says that.
        if (srcEnum.size()) srcCon["enum"] = srcEnum;
        c["source_ip"] = srcCon;

        // Unconstrained, as real kit publishes them. An empty object means "any
        // legal value"; spelling out a 1-65535 range says nothing extra and
        // gives a controller one more thing to disagree with.
        c["destination_ip"]   = Json::object();
        c["destination_port"] = Json::object();
        c["source_port"]      = Json::object();
        c["rtp_enabled"]      = Json::object();
        arr.push(c);
    };
    leg(t.sourceIpA);
    if (t.redundant) leg(t.sourceIpB);
    return arr;
}

// ---- HTTP routes -------------------------------------------------------------

void BuiltinNode::installRoutes() {
    const std::string nv = cfg_.registrationVersion;   // node API version
    const std::string cv = cfg_.connectionVersion;     // connection API version
    const std::string nodeBase = "/x-nmos/node/" + nv;
    const std::string connBase = "/x-nmos/connection/" + cv;
    const std::string single   = connBase + "/single/senders/:id";

    auto ok = [](HttpResponse& res, const Json& j) { res.json(j.dump(2)); };
    auto notFound = [](HttpResponse& res) {
        res.json(R"({"code":404,"error":"Not Found","debug":null})", 404);
    };
    auto isSender = [this](const HttpRequest& r) {
        const auto it = r.params.find("id");
        return it != r.params.end() && it->second == senderId_;
    };

    // Base listings. A controller walks these, so they have to be right.
    server_.route("GET", "/", [=, this](const HttpRequest&, HttpResponse& res) {
        ok(res, listing({"x-nmos/"}));
    });
    server_.route("GET", "/x-nmos", [=, this](const HttpRequest&, HttpResponse& res) {
        ok(res, listing({"node/", "connection/"}));
    });
    server_.route("GET", "/x-nmos/node", [=, this](const HttpRequest&, HttpResponse& res) {
        ok(res, listing({(nv + "/").c_str()}));
    });
    server_.route("GET", "/x-nmos/connection", [=, this](const HttpRequest&, HttpResponse& res) {
        ok(res, listing({(cv + "/").c_str()}));
    });

    // ---- IS-04 Node API ---------------------------------------------------
    server_.route("GET", nodeBase, [=, this](const HttpRequest&, HttpResponse& res) {
        ok(res, listing({"self/", "sources/", "flows/", "devices/",
                         "senders/", "receivers/"}));
    });
    server_.route("GET", nodeBase + "/self", [=, this](const HttpRequest&, HttpResponse& res) {
        std::lock_guard<std::mutex> lk(mutex_);
        ok(res, nodeResource());
    });

    auto collection = [&](const std::string& name, auto builder) {
        server_.route("GET", nodeBase + "/" + name,
                      [=, this](const HttpRequest&, HttpResponse& res) {
            std::lock_guard<std::mutex> lk(mutex_);
            Json a = Json::array();
            a.push(builder());
            ok(res, a);
        });
    };
    collection("devices", [this] { return deviceResource(); });
    collection("sources", [this] { return sourceResource(); });
    collection("flows",   [this] { return flowResource(); });
    collection("senders", [this] { return senderResource(); });

    server_.route("GET", nodeBase + "/receivers",
                  [=, this](const HttpRequest&, HttpResponse& res) {
        ok(res, Json::array());   // this app sends only
    });

    auto single04 = [&](const std::string& name, auto builder, auto idOf) {
        server_.route("GET", nodeBase + "/" + name + "/:id",
                      [=, this](const HttpRequest& req, HttpResponse& res) {
            std::lock_guard<std::mutex> lk(mutex_);
            if (req.params.at("id") != idOf()) { notFound(res); return; }
            ok(res, builder());
        });
    };
    single04("devices", [this] { return deviceResource(); }, [this] { return deviceId_; });
    single04("sources", [this] { return sourceResource(); }, [this] { return sourceId_; });
    single04("flows",   [this] { return flowResource(); },   [this] { return flowId_; });
    single04("senders", [this] { return senderResource(); }, [this] { return senderId_; });

    // ---- IS-05 Connection API ---------------------------------------------
    server_.route("GET", connBase, [=, this](const HttpRequest&, HttpResponse& res) {
        ok(res, listing({"bulk/", "single/"}));
    });
    server_.route("GET", connBase + "/single", [=, this](const HttpRequest&, HttpResponse& res) {
        ok(res, listing({"senders/", "receivers/"}));
    });
    server_.route("GET", connBase + "/single/receivers",
                  [=, this](const HttpRequest&, HttpResponse& res) {
        ok(res, Json::array());
    });
    server_.route("GET", connBase + "/single/senders",
                  [=, this](const HttpRequest&, HttpResponse& res) {
        std::lock_guard<std::mutex> lk(mutex_);
        Json a = Json::array();
        a.push(Json(senderId_ + "/"));
        ok(res, a);
    });
    server_.route("GET", single, [=, this](const HttpRequest& req, HttpResponse& res) {
        if (!isSender(req)) { notFound(res); return; }
        ok(res, listing({"constraints/", "staged/", "active/",
                         "transportfile/", "transporttype/"}));
    });
    // IS-05 v1.1 requires this alongside the others. It reports the transport
    // class, not the sub-classification, so it is rtp rather than rtp.mcast.
    server_.route("GET", single + "/transporttype",
                  [=, this](const HttpRequest& req, HttpResponse& res) {
        if (!isSender(req)) { notFound(res); return; }
        ok(res, Json("urn:x-nmos:transport:rtp"));
    });
    server_.route("GET", single + "/constraints",
                  [=, this](const HttpRequest& req, HttpResponse& res) {
        if (!isSender(req)) { notFound(res); return; }
        ok(res, constraints());
    });
    server_.route("GET", single + "/active",
                  [=, this](const HttpRequest& req, HttpResponse& res) {
        if (!isSender(req)) { notFound(res); return; }
        ok(res, activeObject());
    });
    server_.route("GET", single + "/staged",
                  [=, this](const HttpRequest& req, HttpResponse& res) {
        if (!isSender(req)) { notFound(res); return; }
        ok(res, stagedObject());
    });
    server_.route("PATCH", single + "/staged",
                  [=, this](const HttpRequest& req, HttpResponse& res) {
        if (!isSender(req)) { notFound(res); return; }
        handleStagedPatch(req, res);
    });
    server_.route("GET", single + "/transportfile",
                  [=, this](const HttpRequest& req, HttpResponse& res) {
        if (!isSender(req)) { notFound(res); return; }
        std::lock_guard<std::mutex> lk(mutex_);
        res.status = 200;
        res.contentType = "application/sdp";
        res.body = sdp();
    });

    // Bulk is required by IS-05 for a conformant sender even with one sender.
    server_.route("POST", connBase + "/bulk/senders",
                  [=, this](const HttpRequest& req, HttpResponse& res) {
        std::string err;
        const Json body = Json::parse(req.body, err);
        if (!err.empty() || !body.isArray()) {
            res.json(R"({"code":400,"error":"expected a JSON array","debug":null})", 400);
            return;
        }
        Json out = Json::array();
        for (const Json& entry : body.elements()) {
            Json r = Json::object();
            r["id"] = entry.at("id");
            if (entry.at("id").asString() != senderId_) {
                Json e = Json::object();
                e["code"] = Json(404);
                e["error"] = Json("unknown sender");
                e["debug"] = Json();
                r["code"] = Json(404);
                r["error"] = e;
                out.push(r);
                continue;
            }
            HttpRequest sub;
            sub.method = "PATCH";
            sub.body   = entry.at("params").dump();
            sub.params["id"] = senderId_;
            HttpResponse subRes;
            handleStagedPatch(sub, subRes);
            r["code"] = Json(std::int64_t(subRes.status));
            out.push(r);
        }
        res.json(out.dump(2), 200);
    });
}

void BuiltinNode::handleStagedPatch(const HttpRequest& req, HttpResponse& res) {
    std::string perr;
    const Json body = Json::parse(req.body, perr);
    if (!perr.empty() || !body.isObject()) {
        res.json(R"({"code":400,"error":"invalid JSON body","debug":null})", 400);
        return;
    }

    // A PATCH is a partial update of whatever is already staged, so start from
    // the staged copy if there is one and the live parameters if there is not.
    SenderTransport want;
    bool haveStaged, enable;
    std::string receiverId;
    {
        std::lock_guard<std::mutex> lk(is05Mutex_);
        haveStaged = stagedValid_;
        if (haveStaged) want = staged_;
        enable     = stagedMasterEnable_;
        receiverId = stagedReceiverId_;
    }
    // With nothing staged, `staged` mirrors what is live -- including
    // master_enable. Belt and braces against the desync notifyChanged() fixes:
    // a PATCH that does not mention master_enable must never be able to turn a
    // running sender off as a side effect of moving it.
    if (!haveStaged) {
        want = transport();
        enable = want.active;
    }

    if (body.has("master_enable")) enable = body.at("master_enable").asBool(enable);

    if (body.has("receiver_id")) {
        const Json& rid = body.at("receiver_id");
        receiverId = rid.isNull() ? std::string() : rid.asString();
    }

    if (body.has("transport_params")) {
        const Json& tp = body.at("transport_params");
        if (!tp.isArray()) {
            res.json(R"({"code":400,"error":"transport_params must be an array","debug":null})", 400);
            return;
        }
        const std::size_t legs = want.redundant ? 2u : 1u;
        if (tp.size() > legs) {
            res.json(R"({"code":400,"error":"more transport_params than the sender has legs","debug":null})", 400);
            return;
        }
        for (std::size_t i = 0; i < tp.size(); ++i) {
            const Json& p = tp.elements()[i];
            std::string* dip  = i == 0 ? &want.destIpA : &want.destIpB;
            std::uint16_t* dp = i == 0 ? &want.destPortA : &want.destPortB;
            std::uint16_t* sp = i == 0 ? &want.sourcePortA : &want.sourcePortB;
            if (p.has("destination_ip") && !p.at("destination_ip").isNull())
                *dip = p.at("destination_ip").asString(*dip);
            if (p.has("destination_port") && p.at("destination_port").isNumber())
                *dp = std::uint16_t(p.at("destination_port").asInt(*dp));
            if (p.has("source_port") && p.at("source_port").isNumber())
                *sp = std::uint16_t(p.at("source_port").asInt(*sp));
            if (p.has("rtp_enabled"))
                want.active = p.at("rtp_enabled").asBool(want.active);
            // source_ip is constrained to a single value: the NIC is picked in
            // the GUI. Silently ignoring a change would be worse than saying so.
            if (p.has("source_ip") && !p.at("source_ip").isNull()) {
                const std::string asked = p.at("source_ip").asString();
                const std::string have = i == 0 ? want.sourceIpA : want.sourceIpB;
                if (!have.empty() && asked != have) {
                    res.json(R"({"code":400,"error":"source_ip is fixed by the selected network interface","debug":null})", 400);
                    return;
                }
            }
        }
    }

    // Activation.
    std::string mode;
    if (body.has("activation") && body.at("activation").isObject()) {
        const Json& a = body.at("activation");
        if (a.has("mode") && !a.at("mode").isNull()) mode = a.at("mode").asString();
    }

    if (mode.empty()) {
        {
            std::lock_guard<std::mutex> lk(is05Mutex_);
            staged_ = want;
            stagedValid_ = true;
            stagedMasterEnable_ = enable;
            stagedReceiverId_ = receiverId;
        }
        res.json(stagedObject().dump(2), 200);
        return;
    }
    if (mode != "activate_immediate") {
        res.json(R"({"code":501,"error":"only activate_immediate is supported","debug":"scheduled activation is not implemented"})", 501);
        return;
    }

    want.active = enable;

    // What was asked for, recorded before the attempt, so the status window can
    // show a failed activation rather than only a successful one.
    std::string asked = std::string(enable ? "enable " : "disable ") +
                        want.destIpA + ":" + std::to_string(want.destPortA);
    if (want.redundant)
        asked += " + " + want.destIpB + ":" + std::to_string(want.destPortB);

    std::string err;
    if (apply_ && !apply_(want, err)) {
        setLastActivation(asked + "  -- REJECTED: " +
                          (err.empty() ? std::string("could not apply") : err));
        const Json e = Json(err.empty() ? std::string("could not apply") : err);
        res.json(std::string(R"({"code":500,"error":)") + e.dump() +
                 R"(,"debug":null})", 500);
        return;
    }
    setLastActivation(asked + "  -- applied");

    {
        std::lock_guard<std::mutex> lk(is05Mutex_);
        stagedValid_ = false;
        staged_ = SenderTransport{};
        masterEnable_ = enable;
        activeReceiverId_ = receiverId;
        stagedReceiverId_.clear();
        stagedMasterEnable_ = enable;
    }
    bumpVersion();
    wake();

    Json act = Json::object();
    act["mode"]            = Json("activate_immediate");
    act["requested_time"]  = Json();
    act["activation_time"] = Json(taiVersion());

    Json out = Json::object();
    out["receiver_id"]      = receiverId.empty() ? Json() : Json(receiverId);
    out["master_enable"]    = Json(enable);
    out["activation"]       = act;
    out["transport_params"] = transportParams(transport());
    res.json(out.dump(2), 200);
}

// ---- registration ------------------------------------------------------------

bool BuiltinNode::discoverRegistry() {
    if (!cfg_.registryHost.empty()) {
        std::lock_guard<std::mutex> lk(mutex_);
        registryHost_ = cfg_.registryHost;
        registryPort_ = cfg_.registryPort ? cfg_.registryPort : 3210;
        registryDiscovery_ = "manual override";
        registryServiceType_.clear();
        return true;
    }
    if (!cfg_.useMdns) return false;

    MdnsService best;
    if (!browser_.best(cfg_.registrationVersion, best)) {
        // Something was advertised but cannot be used -- almost always an
        // api_ver that does not list ours. Say which, rather than leaving the
        // status on "looking for a registry" while one is plainly on the wire.
        const std::string why = browser_.rejection(cfg_.registrationVersion);
        std::lock_guard<std::mutex> lk(mutex_);
        mdnsRejection_ = why;
        return false;
    }
    std::lock_guard<std::mutex> lk(mutex_);
    mdnsRejection_.clear();
    registryHost_ = best.address.empty() ? best.hostName : best.address;
    registryPort_ = best.port;
    registryDiscovery_ = "mDNS";
    registryServiceType_ = best.serviceType;
    return !registryHost_.empty() && registryPort_ != 0;
}

bool BuiltinNode::postResource(const char* type, const Json& data, std::string& err) {
    std::string host;
    std::uint16_t port;
    std::string ver;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        host = registryHost_;
        port = registryPort_;
        ver  = cfg_.registrationVersion;
    }
    Json body = Json::object();
    body["type"] = Json(type);
    body["data"] = data;

    const HttpResult r = httpRequest(host, port, "POST",
                                     "/x-nmos/registration/" + ver + "/resource",
                                     body.dump(), "application/json", 5000);
    if (!r.ok) { err = r.error; return false; }
    // 200 = updated, 201 = created. Anything else is a real problem.
    if (r.status != 200 && r.status != 201) {
        err = std::string(type) + ": HTTP " + std::to_string(r.status);
        if (!r.body.empty()) err += " " + r.body.substr(0, 200);
        return false;
    }
    return true;
}

bool BuiltinNode::registerAll() {
    // Order matters: the registry rejects a child whose parent it has not seen.
    struct Item { const char* type; Json data; };
    std::vector<Item> items;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        items = {{"node",   nodeResource()},
                 {"device", deviceResource()},
                 {"source", sourceResource()},
                 {"flow",   flowResource()},
                 {"sender", senderResource()}};
    }
    for (const auto& it : items) {
        std::string err;
        if (!postResource(it.type, it.data, err)) {
            std::lock_guard<std::mutex> lk(mutex_);
            error_ = "registration failed at " + std::string(it.type) + ": " + err;
            registered_ = false;
            return false;
        }
    }
    std::lock_guard<std::mutex> lk(mutex_);
    registered_ = true;
    dirty_ = false;
    error_.clear();
    return true;
}

bool BuiltinNode::heartbeat() {
    std::string host, ver, id;
    std::uint16_t port;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        host = registryHost_;
        port = registryPort_;
        ver  = cfg_.registrationVersion;
        id   = nodeId_;
    }
    const HttpResult r = httpRequest(host, port, "POST",
                                     "/x-nmos/registration/" + ver + "/health/nodes/" + id,
                                     {}, "application/json", 5000);
    std::lock_guard<std::mutex> lk(mutex_);
    if (r.ok && r.status == 200) {
        ++heartbeats_;
        lastHeartbeatOk_ = Clock::now();
        everHeartbeat_ = true;
        error_.clear();
        return true;
    }
    ++heartbeatFailures_;
    // 404 means the registry has forgotten us -- garbage collected after a
    // missed heartbeat, or it restarted. Re-registering is the correct
    // response, and is why this returns false rather than retrying the beat.
    if (r.ok && r.status == 404) {
        registered_ = false;
        error_ = "registry no longer knows this node; re-registering";
    } else {
        error_ = r.ok ? ("heartbeat: HTTP " + std::to_string(r.status))
                      : ("heartbeat: " + r.error);
    }
    return false;
}

void BuiltinNode::unregister() {
    std::string host, ver, id;
    std::uint16_t port;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!registered_) return;
        host = registryHost_;
        port = registryPort_;
        ver  = cfg_.registrationVersion;
        id   = nodeId_;
        registered_ = false;
    }
    // Deleting the node deletes everything beneath it, so a controller sees the
    // sender disappear cleanly instead of waiting for it to be garbage
    // collected after a missed heartbeat.
    httpRequest(host, port, "DELETE",
                "/x-nmos/registration/" + ver + "/resource/nodes/" + id,
                {}, "application/json", 3000);
}

void BuiltinNode::registrationLoop() {
    auto lastBeat = Clock::now() - std::chrono::seconds(60);

    while (!stopping_.load(std::memory_order_relaxed)) {
        bool haveRegistry;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            haveRegistry = !registryHost_.empty() && registryPort_ != 0;
        }
        if (!haveRegistry) {
            if (discoverRegistry()) {
                std::lock_guard<std::mutex> lk(mutex_);
                state_ = "registry " + registryHost_ + ":" +
                         std::to_string(registryPort_);
            } else if (!cfg_.registryHost.empty()) {
                setState("registry override set but unusable");
            } else {
                std::lock_guard<std::mutex> lk(mutex_);
                state_ = mdnsRejection_.empty()
                             ? "looking for a registry over mDNS"
                             : mdnsRejection_;
            }
        }

        bool registered, dirty;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            haveRegistry = !registryHost_.empty() && registryPort_ != 0;
            registered = registered_;
            dirty = dirty_;
        }

        if (haveRegistry && (!registered || dirty)) {
            setState(registered ? "updating registration" : "registering");
            if (registerAll()) {
                std::lock_guard<std::mutex> lk(mutex_);
                state_ = "registered with " + registryHost_ + ":" +
                         std::to_string(registryPort_);
                lastBeat = Clock::now() - std::chrono::seconds(60);
            } else {
                // Give a failing registry a moment rather than hammering it.
                std::unique_lock<std::mutex> lk(mutex_);
                cv_.wait_for(lk, std::chrono::seconds(5));
                // A discovered registry that will not take us may have gone; go
                // back round and look again. A configured override stays put.
                if (cfg_.registryHost.empty()) {
                    registryHost_.clear();
                    registryPort_ = 0;
                    registryDiscovery_.clear();
                    registryServiceType_.clear();
                }
                continue;
            }
        }

        {
            std::lock_guard<std::mutex> lk(mutex_);
            registered = registered_;
        }
        if (registered) {
            const auto due = std::chrono::seconds(std::max(1, cfg_.heartbeatSeconds));
            if (Clock::now() - lastBeat >= due) {
                heartbeat();
                lastBeat = Clock::now();
            }
        }

        std::unique_lock<std::mutex> lk(mutex_);
        cv_.wait_for(lk, std::chrono::milliseconds(500));
    }
    unregister();
}

// ---- lifecycle ---------------------------------------------------------------

bool BuiltinNode::start(const NmosConfig& cfg) {
    stop();
    cfg_ = cfg;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        error_.clear();
        warning_.clear();
        registered_ = false;
        registryHost_.clear();
        registryPort_ = 0;
        registryDiscovery_.clear();
        registryServiceType_.clear();
        mdnsRejection_.clear();
        lastActivation_.clear();
        heartbeats_ = heartbeatFailures_ = 0;
        everHeartbeat_ = false;
        state_ = "starting";
    }

    // Find a port before minting anything, because the identity depends on it.
    //
    // Nothing in IS-04 requires 3210 -- it is a convention, and the node's own
    // href and the DNS-SD advertisement both carry whatever was actually bound,
    // so a controller finds the node either way. That makes stepping to the next
    // free port strictly better than refusing to start, which is what a second
    // instance on one machine used to do.
    requestedPort_ = cfg_.nodePort;
    nodePort_ = cfg_.nodePort;
    if (cfg_.nodePort != 0) {
        const std::uint16_t free = firstFreePort(cfg_.nodeIp, cfg_.nodePort, 20);
        if (free != 0) nodePort_ = free;
        // If the whole span is taken, fall through on the requested port and let
        // start() produce the real bind error rather than inventing one here.
    }
    mintIds();
    const bool liveNow = transport().active;
    {
        std::lock_guard<std::mutex> lk(is05Mutex_);
        stagedValid_ = false;
        staged_ = SenderTransport{};
        masterEnable_ = liveNow;
        stagedMasterEnable_ = liveNow;
        activeReceiverId_.clear();
        stagedReceiverId_.clear();
    }

    installRoutes();
    if (!server_.start(cfg_.nodeIp, nodePort_)) {
        std::lock_guard<std::mutex> lk(mutex_);
        error_ = "node API: " + server_.error();
        state_ = "stopped";
        return false;
    }
    // Re-mint against the port that was actually bound. It can differ from the
    // one probed for -- an ephemeral request, or another process taking the
    // probed port in between -- and the identity has to follow the reality.
    if (server_.port() != nodePort_) {
        nodePort_ = server_.port();
        mintIds();
    }
    if (requestedPort_ != 0 && nodePort_ != requestedPort_) {
        std::lock_guard<std::mutex> lk(mutex_);
        warning_ = "port " + std::to_string(requestedPort_) +
                   " was already in use, so this instance took " +
                   std::to_string(nodePort_) +
                   " and has its own resource IDs";
    }

    if (cfg_.useMdns && cfg_.registryHost.empty()) {
        // Both registration types, because which one a registry advertises
        // depends on the IS-04 versions it serves. See registryServiceTypes().
        if (!browser_.start(registryServiceTypes())) {
            std::lock_guard<std::mutex> lk(mutex_);
            warning_ = "mDNS browse unavailable: " + browser_.error() +
                       " -- set a registry host and port instead";
        }
    }

    if (cfg_.advertisePeerToPeer) {
        std::map<std::string, std::string> txt = {
            {"api_ver",   cfg_.registrationVersion},
            {"api_proto", "http"},
            {"api_auth",  "false"},
            {"ver_slf",   "0"}, {"ver_src", "0"}, {"ver_flw", "0"},
            {"ver_dvc",   "0"}, {"ver_snd", "0"}, {"ver_rcv", "0"},
        };
        // instanceLabel() already carries host and port, which is exactly what
        // DNS-SD needs: instance names have to be unique on the segment, and two
        // copies sharing one would leave the loser silently unadvertised.
        const std::string label = instanceLabel();
        if (!advertiser_.start(label, "_nmos-node._tcp", server_.port(), txt,
                               cfg_.nodeIp)) {
            std::lock_guard<std::mutex> lk(mutex_);
            if (warning_.empty()) warning_ = advertiser_.error();
        }
    }

    stopping_.store(false, std::memory_order_relaxed);
    running_.store(true, std::memory_order_relaxed);
    thread_ = std::thread(&BuiltinNode::registrationLoop, this);
    return true;
}

void BuiltinNode::stop() {
    stopping_.store(true, std::memory_order_relaxed);
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    advertiser_.stop();
    browser_.stop();
    server_.stop();
    running_.store(false, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(mutex_);
    state_ = "stopped";
}

NmosStatus BuiltinNode::status() const {
    NmosStatus s;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        s.running       = running_.load(std::memory_order_relaxed);
        s.error         = error_;
        s.warning       = warning_;
        s.registryState = state_;
        s.registered    = registered_;
        s.heartbeats    = heartbeats_;
        s.heartbeatFailures = heartbeatFailures_;
        s.nodeId   = nodeId_;
        s.deviceId = deviceId_;
        s.sourceId = sourceId_;
        s.flowId   = flowId_;
        s.senderId = senderId_;
        s.groupHint = groupHint();
        s.registryHost      = registryHost_;
        s.registryPort      = registryPort_;
        s.registryDiscovery = registryDiscovery_;
        s.registryServiceType = registryServiceType_;
        s.lastActivation    = lastActivation_;
        s.mdnsRejection     = mdnsRejection_;
        if (!registryHost_.empty())
            s.registryUrl = "http://" + registryHost_ + ":" +
                            std::to_string(registryPort_) + "/x-nmos/registration/" +
                            cfg_.registrationVersion + "/";
        if (everHeartbeat_)
            s.lastHeartbeatAgo =
                std::chrono::duration<double>(Clock::now() - lastHeartbeatOk_).count();
        const std::string host = cfg_.nodeIp.empty() ? hostLabel_ : cfg_.nodeIp;
        s.nodeApiUrl = joinUrl(host, server_.port(),
                               "/x-nmos/node/" + cfg_.registrationVersion + "/");
    }
    s.advertising    = advertiser_.running();
    s.browsing       = browser_.running();
    s.browsedServiceTypes = browser_.serviceTypes();
    s.mdnsError      = browser_.error();
    s.requestsServed = server_.requestsServed();
    s.lastRequest    = server_.lastRequestLine();
    s.masterEnable = transport().active;      // live, not last-commanded
    {
        std::lock_guard<std::mutex> lk(is05Mutex_);
        s.connectedReceiverId = activeReceiverId_;
    }
    for (const auto& m : browser_.found()) {
        std::string line = m.displayName + "  " +
                           (m.address.empty() ? m.hostName : m.address) + ":" +
                           std::to_string(m.port) +
                           "  pri=" + std::to_string(m.priority);
        if (!m.serviceType.empty()) line += "  " + m.serviceType;
        if (!m.apiVersions.empty()) {
            line += "  api_ver=";
            for (std::size_t i = 0; i < m.apiVersions.size(); ++i)
                line += (i ? "," : "") + m.apiVersions[i];
        }
        if (m.port == 0) line += "  (resolving)";
        s.discoveredRegistries.push_back(line);
    }
    return s;
}

}  // namespace

NmosBackend* createBuiltinBackend() { return new BuiltinNode(); }

}  // namespace pcapreplay::nmos
