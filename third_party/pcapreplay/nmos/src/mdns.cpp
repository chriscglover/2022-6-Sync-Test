// The platform-independent half of DNS-SD.
//
// Discovery itself is not portable -- Windows has DnsServiceBrowse/Register in
// dnsapi.dll and Linux has no equivalent short of linking Avahi, which this
// project will not do for the same reason it will not link Bonjour: it would
// stop the deployment being one binary. So the browse and advertise engines live
// in mdns_win.cpp and mdns_posix.cpp, and everything either of them would
// otherwise duplicate lives here: the record interpretation, the list of found
// services, and the choice of which registry to use.
//
// See mdns.h for what DNS-SD is being used for, and mdns_posix.cpp for the
// mechanics of speaking multicast DNS directly.
#include "pcapreplay/nmos/mdns.h"

#include "mdns_internal.h"

#include <algorithm>
#include <cstdlib>

namespace pcapreplay::nmos {

namespace detail {

std::vector<std::string> splitCsv(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else if (c != ' ') {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

void applyTxtRecords(MdnsService& s) {
    if (auto it = s.txt.find("pri"); it != s.txt.end())
        s.priority = std::atoi(it->second.c_str());
    if (auto it = s.txt.find("api_proto"); it != s.txt.end())
        s.apiProto = it->second;
    if (auto it = s.txt.find("api_auth"); it != s.txt.end())
        s.apiAuth = it->second;
    if (auto it = s.txt.find("api_ver"); it != s.txt.end())
        s.apiVersions = splitCsv(it->second);
}

std::string firstLabel(const std::string& name) {
    const std::size_t dot = name.find('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

void deriveNames(MdnsService& s) {
    const std::size_t dot = s.instance.find('.');
    s.displayName = dot == std::string::npos ? s.instance : s.instance.substr(0, dot);
    if (dot == std::string::npos) return;

    s.serviceType = s.instance.substr(dot + 1);
    while (!s.serviceType.empty() && s.serviceType.back() == '.')
        s.serviceType.pop_back();
    const std::string local = ".local";
    if (s.serviceType.size() > local.size() &&
        s.serviceType.compare(s.serviceType.size() - local.size(),
                              local.size(), local) == 0)
        s.serviceType.erase(s.serviceType.size() - local.size());
}

std::string qualify(const std::string& serviceType) {
    std::string q = serviceType;
    if (q.size() < 6 || q.compare(q.size() - 6, 6, ".local") != 0) q += ".local";
    return q;
}

std::string joinOr(const std::vector<std::string>& v, const char* sep) {
    if (v.empty()) return "nothing";
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) out += (i ? sep : "") + v[i];
    return out;
}

}  // namespace detail

// ---------------------------------------------------------------------------

bool MdnsService::supportsVersion(const std::string& v) const {
    // A registry that advertises no api_ver at all is, per IS-04, assumed to
    // support v1.0 only -- but in practice several publish nothing and serve
    // current versions happily, so treat an empty list as "try it".
    if (apiVersions.empty()) return true;
    return std::find(apiVersions.begin(), apiVersions.end(), v) != apiVersions.end();
}

std::string MdnsService::baseUrl(const std::string& apiVersion) const {
    const std::string hostPart = address.empty() ? hostName : address;
    return apiProto + "://" + hostPart + ":" + std::to_string(port) +
           "/x-nmos/registration/" + apiVersion;
}

// ---------------------------------------------------------------------------
// Browser -- the parts that do not touch the network
// ---------------------------------------------------------------------------

MdnsBrowser::~MdnsBrowser() { stop(); }

bool MdnsBrowser::start(const std::string& serviceType) {
    return start(std::vector<std::string>{serviceType});
}

void MdnsBrowser::onInstance(MdnsService s) {
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto& e : found_) {
        if (e.instance == s.instance) { e = std::move(s); return; }
    }
    found_.push_back(std::move(s));
}

void MdnsBrowser::onInstanceGone(const std::string& instance) {
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto it = found_.begin(); it != found_.end(); ++it) {
        if (it->instance == instance) { found_.erase(it); return; }
    }
}

void MdnsBrowser::onError(const std::string& e) {
    std::lock_guard<std::mutex> lk(mutex_);
    error_ = e;
}

std::vector<MdnsService> MdnsBrowser::found() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return found_;
}

bool MdnsBrowser::best(const std::string& apiVersion, MdnsService& out) const {
    std::lock_guard<std::mutex> lk(mutex_);
    const MdnsService* pick = nullptr;
    for (const auto& s : found_) {
        if (s.port == 0) continue;                       // not resolved yet
        if (s.address.empty() && s.hostName.empty()) continue;
        if (!s.supportsVersion(apiVersion)) continue;
        if (!pick || s.priority < pick->priority) pick = &s;
    }
    if (!pick) return false;
    out = *pick;
    return true;
}

std::string MdnsBrowser::rejection(const std::string& apiVersion) const {
    std::lock_guard<std::mutex> lk(mutex_);
    int unresolved = 0;
    std::string wrongVersion;
    for (const auto& s : found_) {
        if (s.port == 0 || (s.address.empty() && s.hostName.empty())) {
            ++unresolved;
            continue;
        }
        if (s.supportsVersion(apiVersion)) return {};   // best() would take it
        if (!wrongVersion.empty()) wrongVersion += ", ";
        wrongVersion += s.displayName + " serves " +
                        detail::joinOr(s.apiVersions, "/");
    }
    if (!wrongVersion.empty())
        return "found a registry but it does not serve " + apiVersion + ": " +
               wrongVersion;
    if (unresolved)
        return std::to_string(unresolved) +
               " registry advertisement(s) seen but not yet resolved";
    return {};
}

std::vector<std::string> MdnsBrowser::serviceTypes() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return serviceTypes_;
}

std::string MdnsBrowser::error() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return error_;
}

// ---------------------------------------------------------------------------

// IS-04 named the Registration API's DNS-SD type _nmos-registration._tcp up to
// v1.2, and added _nmos-register._tcp at v1.3 because the first is 18 characters
// against the 16 RFC 6763 section 7.2 allows. A registry advertises whichever
// suits the versions it serves -- sony/nmos-cpp advertises the long name for
// v1.2 and below and the short one for v1.3 and above -- so both must be
// browsed. Browsing only the v1.3 name is why a registry that is plainly present
// on the network can go unseen.
std::vector<std::string> registryServiceTypes() {
    return {"_nmos-register._tcp", "_nmos-registration._tcp"};
}

// ---------------------------------------------------------------------------

MdnsAdvertiser::~MdnsAdvertiser() { stop(); }

std::string MdnsAdvertiser::error() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return error_;
}

}  // namespace pcapreplay::nmos
