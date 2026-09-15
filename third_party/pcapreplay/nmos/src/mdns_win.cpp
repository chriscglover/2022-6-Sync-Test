// DNS-SD browse and advertise on the Windows native API.
//
// Uses DnsServiceBrowse/DnsServiceResolve/DnsServiceRegister from dnsapi.dll
// (Windows 10 1703 and later) rather than Apple's Bonjour SDK. That keeps the
// app dependency-free and installable by copying it -- the Bonjour SDK is a
// gated download and its runtime is a separate install on every machine.
//
// The Linux half of this is mdns_posix.cpp, which has no equivalent API to call
// and speaks multicast DNS itself. See mdns.h for what either is used for, and
// mdns.cpp for everything the two have in common.
#include "pcapreplay/nmos/mdns.h"

#ifdef _WIN32

#include "mdns_internal.h"

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windns.h>

#include <algorithm>
#include <atomic>
#include <cstdio>

namespace pcapreplay::nmos {
namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(std::size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
    return w;
}

std::string narrow(const wchar_t* w) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(std::size_t(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::string dnsErrorText(DWORD status) {
    switch (status) {
        case ERROR_SUCCESS:            return {};
        case DNS_ERROR_RCODE_NAME_ERROR: return "no such service on this network";
        case ERROR_TIMEOUT:            return "timed out";
        case ERROR_CANCELLED:          return "cancelled";
        case ERROR_INVALID_PARAMETER:  return "invalid parameter";
        case DNS_ERROR_ZONE_DOES_NOT_EXIST: return "zone does not exist";
        default: {
            char buf[64];
            std::snprintf(buf, sizeof buf, "DNS error %lu", status);
            return buf;
        }
    }
}

}  // namespace

std::string localHostLabel() {
    wchar_t buf[256] = {};
    DWORD n = 256;
    if (GetComputerNameW(buf, &n)) return narrow(buf);
    return "pcap-replay";
}

// ---------------------------------------------------------------------------
// Browser
// ---------------------------------------------------------------------------

struct MdnsBrowser::Impl {
    MdnsBrowser*      owner = nullptr;

    // One browse per service type. Each is separately cancellable and carries
    // its own query name, which has to outlive the call because DnsServiceBrowse
    // keeps the pointer.
    struct BrowseCtx {
        Impl*              impl = nullptr;
        std::wstring       queryName;
        DNS_SERVICE_CANCEL cancel{};
        bool               browsing = false;
    };
    std::vector<BrowseCtx*> browses;

    // Resolves are individually cancellable and complete asynchronously, so
    // each carries its own context that outlives this call.
    struct ResolveCtx {
        Impl*            impl = nullptr;
        std::wstring     instance;
        DNS_SERVICE_CANCEL cancel{};
        std::atomic<bool> done{false};
    };
    std::mutex                 resolveMutex;
    std::vector<ResolveCtx*>   resolves;
    std::atomic<bool>          shuttingDown{false};

    static void WINAPI browseCallback(DWORD status, void* ctx, DNS_RECORD* records);
    static void WINAPI resolveCallback(DWORD status, void* ctx,
                                       DNS_SERVICE_INSTANCE* instance);
    void startResolve(const std::wstring& instanceName);
    void reapFinishedResolves();
};

void WINAPI MdnsBrowser::Impl::browseCallback(DWORD status, void* ctx,
                                              DNS_RECORD* records) {
    auto* bc = static_cast<BrowseCtx*>(ctx);
    Impl* impl = bc ? bc->impl : nullptr;
    if (!impl || impl->shuttingDown.load()) {
        if (records) DnsRecordListFree(records, DnsFreeRecordList);
        return;
    }
    if (status != ERROR_SUCCESS) {
        // A browse with nothing on the network reports NAME_ERROR; that is a
        // normal answer, not a failure worth showing.
        if (status != DNS_ERROR_RCODE_NAME_ERROR)
            impl->owner->onError("browse: " + dnsErrorText(status));
        if (records) DnsRecordListFree(records, DnsFreeRecordList);
        return;
    }

    for (DNS_RECORD* r = records; r; r = r->pNext) {
        if (r->wType != DNS_TYPE_PTR || !r->Data.PTR.pNameHost) continue;
        impl->startResolve(r->Data.PTR.pNameHost);
    }
    if (records) DnsRecordListFree(records, DnsFreeRecordList);
    impl->reapFinishedResolves();
}

void WINAPI MdnsBrowser::Impl::resolveCallback(DWORD status, void* ctx,
                                               DNS_SERVICE_INSTANCE* instance) {
    auto* rc = static_cast<ResolveCtx*>(ctx);
    if (!rc) return;
    Impl* impl = rc->impl;

    if (status == ERROR_SUCCESS && instance && impl && !impl->shuttingDown.load()) {
        MdnsService s;
        s.instance = narrow(instance->pszInstanceName);
        s.hostName = narrow(instance->pszHostName);
        s.port     = instance->wPort;

        detail::deriveNames(s);

        if (instance->ip4Address) {
            in_addr a{};
            a.S_un.S_addr = *instance->ip4Address;
            char buf[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &a, buf, sizeof buf);
            s.address = buf;
        }
        for (DWORD i = 0; i < instance->dwPropertyCount; ++i) {
            const std::string k = narrow(instance->keys[i]);
            const std::string v = narrow(instance->values[i]);
            if (k.empty()) continue;
            s.txt[k] = v;
        }
        detail::applyTxtRecords(s);

        impl->owner->onInstance(std::move(s));
    }

    if (instance) DnsServiceFreeInstance(instance);
    rc->done.store(true);
}

void MdnsBrowser::Impl::startResolve(const std::wstring& instanceName) {
    {
        std::lock_guard<std::mutex> lk(resolveMutex);
        for (const auto* r : resolves)
            if (r->instance == instanceName && !r->done.load()) return;  // in flight
    }

    auto* rc = new ResolveCtx();
    rc->impl = this;
    rc->instance = instanceName;

    DNS_SERVICE_RESOLVE_REQUEST req{};
    req.Version        = DNS_QUERY_REQUEST_VERSION1;
    req.InterfaceIndex = 0;
    req.QueryName      = const_cast<PWSTR>(rc->instance.c_str());
    req.pResolveCompletionCallback = &Impl::resolveCallback;
    req.pQueryContext  = rc;

    const DWORD st = DnsServiceResolve(&req, &rc->cancel);
    if (st != DNS_REQUEST_PENDING && st != ERROR_SUCCESS) {
        delete rc;
        return;
    }
    std::lock_guard<std::mutex> lk(resolveMutex);
    resolves.push_back(rc);
}

void MdnsBrowser::Impl::reapFinishedResolves() {
    std::lock_guard<std::mutex> lk(resolveMutex);
    for (auto it = resolves.begin(); it != resolves.end();) {
        if ((*it)->done.load()) {
            delete *it;
            it = resolves.erase(it);
        } else {
            ++it;
        }
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

    impl_ = new Impl();
    impl_->owner = this;

    std::string failures;
    for (const std::string& type : serviceTypes) {
        if (type.empty()) continue;
        auto* bc = new Impl::BrowseCtx();
        bc->impl = impl_;
        // DNS-SD browse names are fully qualified into the mDNS domain.
        const std::string q = detail::qualify(type);
        bc->queryName = widen(q);

        DNS_SERVICE_BROWSE_REQUEST req{};
        req.Version          = DNS_QUERY_REQUEST_VERSION1;
        req.InterfaceIndex   = 0;
        req.QueryName        = const_cast<PWSTR>(bc->queryName.c_str());
        req.pBrowseCallback  = &Impl::browseCallback;
        req.pQueryContext    = bc;

        const DWORD st = DnsServiceBrowse(&req, &bc->cancel);
        if (st != DNS_REQUEST_PENDING && st != ERROR_SUCCESS) {
            if (!failures.empty()) failures += "; ";
            failures += q + ": " + dnsErrorText(st);
            delete bc;
            continue;
        }
        bc->browsing = true;
        impl_->browses.push_back(bc);
    }

    if (impl_->browses.empty()) {
        std::lock_guard<std::mutex> lk(mutex_);
        error_ = "cannot browse: " + (failures.empty() ? std::string("no service type given")
                                                       : failures);
        delete impl_;
        impl_ = nullptr;
        return false;
    }
    // Some browses started, so discovery works; the rest are reported but do
    // not sink it. Losing one of the two registration types still finds a
    // registry advertising the other.
    if (!failures.empty()) {
        std::lock_guard<std::mutex> lk(mutex_);
        error_ = "partial browse: " + failures;
    }
    running_ = true;
    return true;
}

void MdnsBrowser::stop() {
    if (!impl_) { running_ = false; return; }
    impl_->shuttingDown.store(true);

    for (auto* bc : impl_->browses) {
        if (bc->browsing) {
            DnsServiceBrowseCancel(&bc->cancel);
            bc->browsing = false;
        }
    }
    {
        std::lock_guard<std::mutex> lk(impl_->resolveMutex);
        for (auto* r : impl_->resolves) {
            if (!r->done.load()) DnsServiceResolveCancel(&r->cancel);
        }
    }
    // Cancellation is asynchronous; give in-flight callbacks a moment to land
    // before the contexts they reference go away.
    Sleep(150);
    {
        std::lock_guard<std::mutex> lk(impl_->resolveMutex);
        for (auto* r : impl_->resolves) delete r;
        impl_->resolves.clear();
    }
    for (auto* bc : impl_->browses) delete bc;
    impl_->browses.clear();
    delete impl_;
    impl_ = nullptr;
    running_ = false;
}


// ---------------------------------------------------------------------------
// Advertiser
// ---------------------------------------------------------------------------

struct MdnsAdvertiser::Impl {
    MdnsAdvertiser*        owner = nullptr;
    PDNS_SERVICE_INSTANCE  instance = nullptr;
    std::atomic<bool>      registered{false};
    std::atomic<bool>      settled{false};

    static void WINAPI registerCallback(DWORD status, void* ctx,
                                        DNS_SERVICE_INSTANCE* inst) {
        auto* impl = static_cast<Impl*>(ctx);
        if (impl) {
            if (status == ERROR_SUCCESS) {
                impl->registered.store(true);
            } else if (impl->owner) {
                std::lock_guard<std::mutex> lk(impl->owner->mutex_);
                impl->owner->error_ = "advertise failed: " + dnsErrorText(status);
            }
            impl->settled.store(true);
        }
        // The callback owns the instance it is handed back.
        if (inst) DnsServiceFreeInstance(inst);
    }
};

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

    const std::string type = detail::qualify(serviceType);

    // DNS-SD instance names must be unique on the link; the host name keeps two
    // copies of this app on different machines from colliding.
    const std::string full = instanceLabel + "." + type;
    const std::string host = localHostLabel() + ".local";

    const std::wstring wFull = widen(full);
    const std::wstring wHost = widen(host);

    std::vector<std::wstring> keyStore, valStore;
    keyStore.reserve(txt.size());
    valStore.reserve(txt.size());
    for (const auto& kv : txt) {
        keyStore.push_back(widen(kv.first));
        valStore.push_back(widen(kv.second));
    }
    std::vector<PCWSTR> keys, vals;
    keys.reserve(txt.size());
    vals.reserve(txt.size());
    for (std::size_t i = 0; i < keyStore.size(); ++i) {
        keys.push_back(keyStore[i].c_str());
        vals.push_back(valStore[i].c_str());
    }

    IP4_ADDRESS ip4 = 0;
    PIP4_ADDRESS pip4 = nullptr;
    if (!hostIpv4.empty()) {
        in_addr a{};
        if (inet_pton(AF_INET, hostIpv4.c_str(), &a) == 1) {
            ip4 = a.S_un.S_addr;
            pip4 = &ip4;
        }
    }

    impl_ = new Impl();
    impl_->owner = this;
    impl_->instance = DnsServiceConstructInstance(
        wFull.c_str(), wHost.c_str(), pip4, nullptr, port,
        /*priority*/ 0, /*weight*/ 0,
        DWORD(keys.size()), keys.empty() ? nullptr : keys.data(),
        vals.empty() ? nullptr : vals.data());

    if (!impl_->instance) {
        std::lock_guard<std::mutex> lk(mutex_);
        error_ = "cannot construct the mDNS service instance";
        delete impl_;
        impl_ = nullptr;
        return false;
    }

    DNS_SERVICE_REGISTER_REQUEST req{};
    req.Version         = DNS_QUERY_REQUEST_VERSION1;
    req.InterfaceIndex  = 0;
    req.pServiceInstance = impl_->instance;
    req.pRegisterCompletionCallback = &Impl::registerCallback;
    req.pQueryContext   = impl_;
    req.unicastEnabled  = FALSE;

    const DWORD st = DnsServiceRegister(&req, nullptr);
    if (st != DNS_REQUEST_PENDING && st != ERROR_SUCCESS) {
        std::lock_guard<std::mutex> lk(mutex_);
        error_ = "cannot advertise " + full + ": " + dnsErrorText(st) +
                 " (peer-to-peer discovery unavailable; registry registration "
                 "is unaffected)";
        DnsServiceFreeInstance(impl_->instance);
        delete impl_;
        impl_ = nullptr;
        return false;
    }
    running_ = true;
    return true;
}

void MdnsAdvertiser::stop() {
    if (!impl_) { running_ = false; return; }

    if (impl_->registered.load() && impl_->instance) {
        DNS_SERVICE_REGISTER_REQUEST req{};
        req.Version          = DNS_QUERY_REQUEST_VERSION1;
        req.InterfaceIndex   = 0;
        req.pServiceInstance = impl_->instance;
        req.pRegisterCompletionCallback = &Impl::registerCallback;
        req.pQueryContext    = nullptr;
        req.unicastEnabled   = FALSE;
        DnsServiceDeRegister(&req, nullptr);
        Sleep(120);            // let the goodbye packet go out
    }
    // The register/deregister callbacks free the instance they are handed; the
    // one constructed here is only ours to free if neither ever ran.
    if (!impl_->settled.load() && impl_->instance)
        DnsServiceFreeInstance(impl_->instance);

    delete impl_;
    impl_ = nullptr;
    running_ = false;
}

}  // namespace pcapreplay::nmos

#endif   // _WIN32
