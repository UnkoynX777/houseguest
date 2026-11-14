#include "houseguest/core/protection.hpp"

#include <algorithm>
#include <array>

#include "houseguest/core/glob.hpp"
#include "houseguest/core/subject.hpp"

namespace houseguest::core {
namespace {

enum class Need { None, Any, Windows };

struct Listed {
    std::string_view relative;
    ProtectedKind kind;
    Need signer;
};

constexpr auto kSystemRoot = std::to_array<Listed>({
    {"system32/csrss.exe", ProtectedKind::WindowsCore, Need::None},
    {"system32/smss.exe", ProtectedKind::WindowsCore, Need::None},
    {"system32/wininit.exe", ProtectedKind::WindowsCore, Need::None},
    {"system32/winlogon.exe", ProtectedKind::WindowsCore, Need::None},
    {"system32/services.exe", ProtectedKind::WindowsCore, Need::None},
    {"system32/lsass.exe", ProtectedKind::WindowsCore, Need::None},
    {"system32/lsaiso.exe", ProtectedKind::WindowsCore, Need::None},
    {"system32/svchost.exe", ProtectedKind::WindowsCore, Need::None},
    {"system32/fontdrvhost.exe", ProtectedKind::WindowsCore, Need::None},
    {"system32/dwm.exe", ProtectedKind::WindowsCore, Need::None},
    {"system32/audiodg.exe", ProtectedKind::WindowsCore, Need::None},
    {"system32/wudfhost.exe", ProtectedKind::WindowsCore, Need::None},
    {"system32/conhost.exe", ProtectedKind::WindowsCore, Need::None},
    {"system32/dllhost.exe", ProtectedKind::WindowsCore, Need::None},
    {"system32/userinit.exe", ProtectedKind::WindowsCore, Need::None},
    {"system32/logonui.exe", ProtectedKind::WindowsCore, Need::None},
    {"system32/consent.exe", ProtectedKind::WindowsCore, Need::None},
    {"system32/credentialuibroker.exe", ProtectedKind::WindowsCore, Need::None},
    {"system32/securityhealthsystray.exe", ProtectedKind::WindowsCore, Need::None},
    {"system32/securityhealthservice.exe", ProtectedKind::WindowsCore, Need::None},
    {"system32/smartscreen.exe", ProtectedKind::WindowsCore, Need::None},
    {"system32/sihost.exe", ProtectedKind::WindowsShell, Need::None},
    {"system32/taskhostw.exe", ProtectedKind::WindowsShell, Need::None},
    {"system32/ctfmon.exe", ProtectedKind::WindowsShell, Need::None},
    {"system32/runtimebroker.exe", ProtectedKind::WindowsShell, Need::None},
    {"system32/applicationframehost.exe", ProtectedKind::WindowsShell, Need::None},
    {"system32/backgroundtaskhost.exe", ProtectedKind::WindowsShell, Need::None},
    {"system32/shellhost.exe", ProtectedKind::WindowsShell, Need::None},
    {"explorer.exe", ProtectedKind::WindowsShell, Need::None},
    {"system32/osk.exe", ProtectedKind::Input, Need::None},
    {"system32/narrator.exe", ProtectedKind::Input, Need::None},
    {"system32/magnify.exe", ProtectedKind::Input, Need::None},
    {"system32/utilman.exe", ProtectedKind::Input, Need::None},
    {"system32/atbroker.exe", ProtectedKind::Input, Need::None},
    {"system32/imebroker.exe", ProtectedKind::Input, Need::None},
    {"system32/ime/**/*.exe", ProtectedKind::Input, Need::Any},
    {"system32/driverstore/filerepository/**/*.exe", ProtectedKind::DriverPackage, Need::Any},
    {"syswow64/dllhost.exe", ProtectedKind::WindowsCore, Need::None},
    {"syswow64/conhost.exe", ProtectedKind::WindowsCore, Need::None},
    {"splwow64.exe", ProtectedKind::Infrastructure, Need::Windows},
    {"system32/werfault.exe", ProtectedKind::Infrastructure, Need::Windows},
    {"system32/wermgr.exe", ProtectedKind::Infrastructure, Need::Windows},
    {"system32/comppkgsrv.exe", ProtectedKind::Infrastructure, Need::Windows},
    {"system32/oobe/useroobebroker.exe", ProtectedKind::Infrastructure, Need::Windows},
    {"system32/searchindexer.exe", ProtectedKind::Infrastructure, Need::Windows},
    {"system32/searchprotocolhost.exe", ProtectedKind::Infrastructure, Need::Windows},
    {"system32/searchfilterhost.exe", ProtectedKind::Infrastructure, Need::Windows},
    {"system32/wlrmdr.exe", ProtectedKind::Infrastructure, Need::Windows},
    {"system32/dashost.exe", ProtectedKind::Infrastructure, Need::Windows},
    {"system32/pickerhost.exe", ProtectedKind::Infrastructure, Need::Windows},
    {"system32/openwith.exe", ProtectedKind::Infrastructure, Need::Windows},
    {"system32/sihclient.exe", ProtectedKind::Infrastructure, Need::Windows},
    {"system32/musnotification.exe", ProtectedKind::Infrastructure, Need::Windows},
    {"system32/musnotifyicon.exe", ProtectedKind::Infrastructure, Need::Windows},
    {"system32/printfilterpipelinesvc.exe", ProtectedKind::Infrastructure, Need::Windows},
    {"system32/printisolationhost.exe", ProtectedKind::Infrastructure, Need::Windows},
    {"system32/spoolsv.exe", ProtectedKind::Infrastructure, Need::Windows},
    {"system32/rdpclip.exe", ProtectedKind::Infrastructure, Need::Windows},
    {"systemapps/**/*.exe", ProtectedKind::ShellApp, Need::Any},
});

constexpr std::string_view kTouchKeyboard = "common files/microsoft shared/ink/tabtip.exe";

constexpr auto kSensitive = std::to_array<std::string_view>({
    "cmd.exe",
    "powershell.exe",
    "pwsh.exe",
    "powershell_ise.exe",
    "wscript.exe",
    "cscript.exe",
    "mshta.exe",
    "rundll32.exe",
    "regsvr32.exe",
    "msiexec.exe",
    "certutil.exe",
    "bitsadmin.exe",
    "wmic.exe",
    "reg.exe",
    "regedit.exe",
    "mmc.exe",
    "taskmgr.exe",
    "schtasks.exe",
    "at.exe",
    "sc.exe",
    "net.exe",
    "net1.exe",
    "curl.exe",
    "wsl.exe",
    "bash.exe",
    "installutil.exe",
    "msbuild.exe",
    "cmstp.exe",
    "forfiles.exe",
    "pcalua.exe",
    "odbcconf.exe",
    "control.exe",
    "wt.exe",
    "eventvwr.exe",
    "systemsettings.exe",
    "runas.exe",
    "takeown.exe",
    "icacls.exe",
    "cacls.exe",
    "netsh.exe",
    "psexec.exe",
    "expand.exe",
    "extrac32.exe",
    "esentutl.exe",
    "certreq.exe",
    "ftp.exe",
    "makecab.exe",
    "msdt.exe",
    "scriptrunner.exe",
    "ieexec.exe",
    "infdefaultinstall.exe",
    "mavinject.exe",
    "regasm.exe",
    "regsvcs.exe",
    "presentationhost.exe",
    "diskshadow.exe",
    "vssadmin.exe",
    "bcdedit.exe",
    "diskpart.exe",
    "wevtutil.exe",
    "sdbinst.exe",
    "hh.exe",
    "dism.exe",
});

std::string_view file_name(std::string_view normalized) {
    const auto slash = normalized.rfind('/');
    return slash == std::string_view::npos ? normalized : normalized.substr(slash + 1);
}

}  // namespace

namespace {

Protection::Signer signer_of(Need need) {
    switch (need) {
        case Need::None:
            return Protection::Signer::None;
        case Need::Any:
            return Protection::Signer::Any;
        case Need::Windows:
            return Protection::Signer::Windows;
    }
    return Protection::Signer::Windows;
}

}  // namespace

Protection Protection::windows_default(const std::string& system_root, const std::string& program_files) {
    Protection protection;
    const auto root = normalize_path(system_root);
    for (const auto& item : kSystemRoot) {
        protection.entries_.push_back({root + "/" + std::string(item.relative), item.kind, signer_of(item.signer)});
    }
    protection.entries_.push_back(
        {normalize_path(program_files) + "/" + std::string(kTouchKeyboard), ProtectedKind::Input, Signer::None});
    protection.com_areas_ = {root + "/system32/*.exe", root + "/system32/**/*.exe", root + "/syswow64/*.exe",
                             root + "/syswow64/**/*.exe"};
    protection.dcom_launcher_ = root + "/system32/svchost.exe";
    return protection;
}

bool is_windows_signer(std::string_view publisher) {
    return subject_has(publisher, "cn", "microsoft windows") ||
           subject_has(publisher, "cn", "microsoft windows publisher");
}

bool Protection::may_be_com_server(std::string_view path) const {
    return !is_sensitive_tool(path) && std::ranges::any_of(com_areas_, [&](const std::string& pattern) {
               return path_matches(pattern, path);
           });
}

std::optional<ProtectedHit> Protection::classify_by_evidence(const ProcessFacts& facts) const {
    if (!facts.com_server || facts.parent_path.empty() || !path_matches(dcom_launcher_, facts.parent_path) ||
        !may_be_com_server(facts.app.path))
        return std::nullopt;
    const auto name = protected_kind_name(ProtectedKind::ComServer);
    if (!facts.publisher_resolved) return ProtectedHit{ProtectedKind::ComServer, name, true};
    if (!is_windows_signer(facts.app.publisher)) return std::nullopt;
    return ProtectedHit{ProtectedKind::ComServer, name, false};
}

std::optional<ProtectedHit> Protection::classify(const ProcessFacts& facts) const {
    if (facts.kernel_critical) return ProtectedHit{ProtectedKind::KernelCritical, "kernel_critical", false};
    if (facts.protected_process)
        return ProtectedHit{ProtectedKind::ProtectedProcess, "protected_process", false};
    if (!facts.image_known || facts.remote_image) return std::nullopt;
    for (const auto& entry : entries_) {
        if (!path_matches(entry.pattern, facts.app.path)) continue;
        if (entry.signer == Signer::None)
            return ProtectedHit{entry.kind, protected_kind_name(entry.kind), false};
        if (!facts.publisher_resolved) return ProtectedHit{entry.kind, protected_kind_name(entry.kind), true};
        const bool signed_as_required = entry.signer == Signer::Windows ? is_windows_signer(facts.app.publisher)
                                                                         : !facts.app.publisher.empty();
        if (signed_as_required) return ProtectedHit{entry.kind, protected_kind_name(entry.kind), false};
        return std::nullopt;
    }
    return classify_by_evidence(facts);
}

bool Protection::covers(std::string_view path) const {
    return std::ranges::any_of(entries_,
                               [&](const Entry& entry) { return path_matches(entry.pattern, path); });
}

std::vector<std::string> Protection::patterns() const {
    std::vector<std::string> out;
    out.reserve(entries_.size());
    for (const auto& entry : entries_) out.push_back(entry.pattern);
    return out;
}

std::string_view protected_kind_name(ProtectedKind kind) {
    switch (kind) {
        case ProtectedKind::KernelCritical:
            return "kernel_critical";
        case ProtectedKind::ProtectedProcess:
            return "protected_process";
        case ProtectedKind::WindowsCore:
            return "windows_core";
        case ProtectedKind::WindowsShell:
            return "windows_shell";
        case ProtectedKind::Input:
            return "input_and_accessibility";
        case ProtectedKind::DriverPackage:
            return "driver_package";
        case ProtectedKind::ShellApp:
            return "shell_app";
        case ProtectedKind::Infrastructure:
            return "system_infrastructure";
        case ProtectedKind::ComServer:
            return "windows_com_server";
    }
    return "unknown";
}

std::span<const std::string_view> sensitive_tool_names() {
    return kSensitive;
}

bool is_sensitive_tool(std::string_view path) {
    const auto normalized = normalize_path(path);
    const auto name = file_name(normalized);
    if (std::ranges::find(kSensitive, name) != kSensitive.end()) return true;
    return name.starts_with("setup") || name.starts_with("unins") ||
           name.find("installer") != std::string_view::npos;
}

}  // namespace houseguest::core
