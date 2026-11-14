#include "houseguest/system/capabilities.hpp"

#include <windows.h>

#include <evntrace.h>
#include <wtsapi32.h>

#include <algorithm>

#include "houseguest/system/crypto.hpp"
#include "houseguest/system/process.hpp"
#include "houseguest/system/text.hpp"
#include "houseguest/system/windows_info.hpp"

#include "win_util.hpp"

namespace houseguest::system {
namespace {

constexpr int kRecommendedBuild = 19041;
constexpr int kMinimumBuild = 10240;
constexpr wchar_t kVersionKey[] = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
constexpr wchar_t kWebViewKey[] =
    L"SOFTWARE\\Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}";
constexpr wchar_t kProbeSession[] = L"HouseguestCapabilityProbe";

std::string registry_text(HKEY root, const wchar_t* key, const wchar_t* value, REGSAM view = 0) {
    wchar_t buffer[256] = {};
    DWORD size = sizeof(buffer) - sizeof(wchar_t);
    const DWORD flags = RRF_RT_REG_SZ | (view == KEY_WOW64_32KEY ? RRF_SUBKEY_WOW6432KEY : 0);
    if (RegGetValueW(root, key, value, flags, nullptr, buffer, &size) != ERROR_SUCCESS) return {};
    return to_utf8(buffer);
}

CapabilityCheck make(std::string id, Health health, bool critical, std::string detail) {
    return {std::move(id), health, critical, std::move(detail)};
}

CapabilityCheck check_build(int build) {
    if (build >= kRecommendedBuild)
        return make("windows_build", Health::Ok, true, "Build " + std::to_string(build));
    if (build >= kMinimumBuild) {
        return make("windows_build", Health::Degraded, true,
                    "Build " + std::to_string(build) + " is older than the tested 19041 and later releases.");
    }
    return make("windows_build", Health::Missing, true, "Windows 10 or later is required.");
}

CapabilityCheck check_crypto() {
    const std::string text = "houseguest";
    const Bytes data(text.begin(), text.end());
    const bool hashing = sha256(data).size() == 32;
    const auto protected_blob = dpapi_protect_machine(data);
    const auto roundtrip = protected_blob.empty() ? std::nullopt : dpapi_unprotect(protected_blob);
    const bool dpapi = roundtrip && *roundtrip == data;
    if (hashing && dpapi)
        return make("cryptography", Health::Ok, true, "Hashing and machine key protection work.");
    return make("cryptography", Health::Missing, true,
                "Machine key protection is unavailable. Signed settings cannot be stored safely.");
}

bool service_installed(const wchar_t* name) {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) return false;
    SC_HANDLE service = OpenServiceW(manager, name, SERVICE_QUERY_STATUS);
    const bool found = service != nullptr;
    if (service) CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return found;
}

CapabilityCheck check_signatures() {
    const bool library =
        GetModuleHandleW(L"wintrust.dll") != nullptr || LoadLibraryW(L"wintrust.dll") != nullptr;
    if (!library) {
        return make("signature_verification", Health::Missing, false,
                    "Publisher rules are unavailable. Path and hash rules still work.");
    }
    if (!service_installed(L"CryptSvc")) {
        return make("signature_verification", Health::Degraded, false,
                    "The Cryptographic Services service is missing. Catalog-signed Windows files cannot be "
                    "attributed to a publisher; path and hash rules still work.");
    }
    return make("signature_verification", Health::Ok, false, "Publisher rules are available.");
}

CapabilityCheck check_sessions() {
    WTS_SESSION_INFOW* sessions = nullptr;
    DWORD count = 0;
    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count)) {
        return make("session_control", Health::Degraded, false,
                    "Guest sessions cannot be disconnected or signed out when time is up.");
    }
    WTSFreeMemory(sessions);
    return make("session_control", Health::Ok, false, "Sessions can be disconnected or signed out.");
}

CapabilityCheck check_accounts() {
    return list_local_users().empty()
               ? make("local_accounts", Health::Missing, false, "Local accounts could not be listed.")
               : make("local_accounts", Health::Ok, false, "Local accounts are available.");
}

CapabilityCheck check_webview() {
    const bool found = !registry_text(HKEY_LOCAL_MACHINE, kWebViewKey, L"pv", KEY_WOW64_32KEY).empty() ||
                       !registry_text(HKEY_LOCAL_MACHINE, kWebViewKey, L"pv").empty() ||
                       !registry_text(HKEY_CURRENT_USER, kWebViewKey, L"pv").empty();
    if (found)
        return make("webview2_runtime", Health::Ok, false, "Microsoft Edge WebView2 Runtime is installed.");
    return make(
        "webview2_runtime", Health::Missing, false,
        "The interface needs the Microsoft Edge WebView2 Runtime. Protection and the command line do not.");
}

CapabilityCheck check_process_events() {
    std::vector<std::uint8_t> buffer(sizeof(EVENT_TRACE_PROPERTIES) + sizeof(kProbeSession));
    auto* properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buffer.data());
    properties->Wnode.BufferSize = static_cast<ULONG>(buffer.size());
    properties->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    properties->Wnode.ClientContext = 2;
    properties->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    properties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    TRACEHANDLE session = 0;
    ULONG status = StartTraceW(&session, kProbeSession, properties);
    if (status == ERROR_ALREADY_EXISTS) {
        ControlTraceW(0, kProbeSession, properties, EVENT_TRACE_CONTROL_STOP);
        status = StartTraceW(&session, kProbeSession, properties);
    }
    if (status != ERROR_SUCCESS) {
        return make("process_events", Health::Degraded, false,
                    "Event Tracing for Windows is unavailable (code " + std::to_string(status) +
                        "). Houseguest falls back to polling, which reacts up to about one second later.");
    }
    ControlTraceW(session, nullptr, properties, EVENT_TRACE_CONTROL_STOP);
    return make("process_events", Health::Ok, false, "Process creation events are available.");
}

CapabilityCheck check_process_access() {
    const auto self = ProcessRef::open(GetCurrentProcessId());
    if (self && self->user_sid() && self->image_path()) {
        return make("process_inspection", Health::Ok, true, "Processes can be inspected and stopped.");
    }
    return make("process_inspection", Health::Missing, true, "Processes cannot be inspected.");
}

}  // namespace

bool Capabilities::critical_failure() const {
    return std::ranges::any_of(checks, [](const CapabilityCheck& check) {
        return check.critical && check.health == Health::Missing;
    });
}

Capabilities probe_capabilities() {
    Capabilities result;
    result.product_name = registry_text(HKEY_LOCAL_MACHINE, kVersionKey, L"ProductName");
    result.edition = registry_text(HKEY_LOCAL_MACHINE, kVersionKey, L"EditionID");
    result.display_version = registry_text(HKEY_LOCAL_MACHINE, kVersionKey, L"DisplayVersion");
    if (result.display_version.empty())
        result.display_version = registry_text(HKEY_LOCAL_MACHINE, kVersionKey, L"ReleaseId");
    result.build = registry_text(HKEY_LOCAL_MACHINE, kVersionKey, L"CurrentBuildNumber");
    const int build = result.build.empty() ? 0 : std::atoi(result.build.c_str());
    result.supported_build = build >= kRecommendedBuild;
    result.checks = {check_build(build), check_crypto(),   check_process_access(), check_process_events(),
                     check_signatures(), check_sessions(), check_accounts(),       check_webview()};
    result.checks.push_back(
        make("defender_independent", Health::Ok, false,
             "Houseguest does not use Windows Defender, SmartScreen, AppLocker or App Control."));
    return result;
}

core::Json to_json(const Capabilities& capabilities) {
    const auto name = [](Health health) {
        return health == Health::Ok ? "ok" : health == Health::Degraded ? "degraded" : "missing";
    };
    core::Json checks = core::Json::array();
    for (const auto& check : capabilities.checks) {
        checks.push_back({{"id", check.id},
                          {"health", name(check.health)},
                          {"critical", check.critical},
                          {"detail", check.detail}});
    }
    return {{"product_name", capabilities.product_name},
            {"edition", capabilities.edition},
            {"display_version", capabilities.display_version},
            {"build", capabilities.build},
            {"supported_build", capabilities.supported_build},
            {"ready", !capabilities.critical_failure()},
            {"webview2", std::ranges::any_of(capabilities.checks,
                                             [](const CapabilityCheck& c) {
                                                 return c.id == "webview2_runtime" && c.health == Health::Ok;
                                             })},
            {"checks", std::move(checks)}};
}

}  // namespace houseguest::system
