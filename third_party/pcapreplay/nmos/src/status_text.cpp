#include "pcapreplay/nmos/status_text.h"

#include <cstdio>
#include <string>

namespace pcapreplay::nmos {
namespace {

std::string commas(std::uint64_t v) {
    std::string s = std::to_string(v);
    for (int i = int(s.size()) - 3; i > 0; i -= 3) s.insert(std::size_t(i), ",");
    return s;
}

}  // namespace

std::string statusText(const NmosStatus& n, bool enabled, const char* eol) {
    if (!enabled) return "NMOS off.";
    if (!n.running) return n.error.empty() ? "NMOS stopped." : ("!! " + n.error);

    std::string s;

    s += std::string("Registered : ") + (n.registered ? "YES" : "no");
    if (n.registered) {
        s += "   heartbeats " + commas(n.heartbeats);
        if (n.lastHeartbeatAgo >= 0.0) {
            char age[32];
            std::snprintf(age, sizeof age, "%.0fs ago", n.lastHeartbeatAgo);
            s += ", last " + std::string(age);
        }
    }
    if (n.heartbeatFailures) s += "   failures " + commas(n.heartbeatFailures);
    s += eol;

    // Where. Only meaningful once a registry has been settled on, so say so
    // plainly when there is not one rather than printing an empty field.
    s += "Registry   : ";
    if (!n.registryUrl.empty()) {
        s += n.registryUrl;
        if (!n.registryDiscovery.empty()) {
            s += "   (via " + n.registryDiscovery;
            if (!n.registryServiceType.empty()) s += " " + n.registryServiceType;
            s += ")";
        }
    } else {
        s += "none yet   -- " + n.registryState;
    }
    s += eol;
    if (!n.registryUrl.empty()) s += "State      : " + n.registryState + eol;

    s += "Node API   : " + n.nodeApiUrl + eol;
    s += std::string("Sender     : ") + (n.masterEnable ? "enabled" : "disabled");
    if (!n.connectedReceiverId.empty()) s += "   routed to " + n.connectedReceiverId;
    s += n.advertising ? "   (advertising peer-to-peer)" : "";
    s += eol;
    s += "Sender id  : " + n.senderId + eol;
    if (!n.groupHint.empty()) s += "Group hint : " + n.groupHint + eol;

    if (!n.lastActivation.empty()) s += "Last IS-05 : " + n.lastActivation + eol;

    // Discovery. Shown even when a registry has been found, because "browsing
    // these types, found these" is what settles an argument about whether mDNS
    // is working on the segment.
    if (n.browsing || !n.browsedServiceTypes.empty()) {
        s += "mDNS       : browsing ";
        for (std::size_t i = 0; i < n.browsedServiceTypes.size(); ++i)
            s += (i ? ", " : "") + n.browsedServiceTypes[i];
        if (!n.browsing) s += "   (stopped)";
        s += eol;
    }
    if (!n.discoveredRegistries.empty()) {
        s += "Found      : ";
        for (std::size_t i = 0; i < n.discoveredRegistries.size(); ++i)
            s += (i ? std::string(eol) + "             " : std::string()) +
                 n.discoveredRegistries[i];
        s += eol;
    } else if (n.browsing) {
        s += "Found      : no registry advertised on this segment yet";
        s += eol;
    }

    if (!n.mdnsRejection.empty()) s += "!! " + n.mdnsRejection + eol;
    if (!n.mdnsError.empty())     s += "!! mDNS: " + n.mdnsError + eol;
    if (!n.warning.empty())       s += "!! " + n.warning + eol;
    if (!n.error.empty())         s += "!! " + n.error + eol;
    return s;
}

}  // namespace pcapreplay::nmos
