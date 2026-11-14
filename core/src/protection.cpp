#include "houseguest/core/protection.hpp"

#include <algorithm>
#include <array>

#include "houseguest/core/glob.hpp"

namespace houseguest::core {
namespace {

struct Listed {
    std::string_view relative;
    ProtectedKind kind;
    bool signature_required;
};

constexpr auto kSystemRoot = std::to_array<Listed>({
    {"system32/csrss.exe", ProtectedKind::WindowsCore, false},
    {"system32/smss.exe", ProtectedKind::WindowsCore, false},
    {"system32/wininit.exe", ProtectedKind::WindowsCore, false},
    {"system32/winlogon.exe", ProtectedKind::WindowsCore, false},
    {"system32/services.exe", ProtectedKind::WindowsCore, false},
    {"system32/lsass.exe", ProtectedKind::WindowsCore, false},
    {"system32/lsaiso.exe", ProtectedKind::WindowsCore, false},
    {"system32/svchost.exe", ProtectedKind::WindowsCore, false},
    {"system32/fontdrvhost.exe", ProtectedKind::WindowsCore, false},
    {"system32/dwm.exe", ProtectedKind::WindowsCore, false},
    {"system32/audiodg.exe", ProtectedKind::WindowsCore, false},
    {"system32/wudfhost.exe", ProtectedKind::WindowsCore, false},
    {"system32/conhost.exe", ProtectedKind::WindowsCore, false},
    {"system32/dllhost.exe", ProtectedKind::WindowsCore, false},
    {"system32/userinit.exe", ProtectedKind::WindowsCore, false},
    {"system32/logonui.exe", ProtectedKind::WindowsCore, false},
    {"system32/consent.exe", ProtectedKind::WindowsCore, false},
    {"system32/credentialuibroker.exe", ProtectedKind::WindowsCore, false},
    {"system32/securityhealthsystray.exe", ProtectedKind::WindowsCore, false},
    {"system32/securityhealthservice.exe", ProtectedKind::WindowsCore, false},
    {"system32/smartscreen.exe", ProtectedKind::WindowsCore, false},
    {"system32/sihost.exe", ProtectedKind::WindowsShell, false},
    {"system32/taskhostw.exe", ProtectedKind::WindowsShell, false},
    {"system32/ctfmon.exe", ProtectedKind::WindowsShell, false},
    {"system32/runtimebroker.exe", ProtectedKind::WindowsShell, false},
    {"system32/applicationframehost.exe", ProtectedKind::WindowsShell, false},
    {"system32/backgroundtaskhost.exe", ProtectedKind::WindowsShell, false},
    {"system32/shellhost.exe", ProtectedKind::WindowsShell, false},
    {"explorer.exe", ProtectedKind::WindowsShell, false},
    {"system32/osk.exe", ProtectedKind::Input, false},
    {"system32/narrator.exe", ProtectedKind::Input, false},
    {"system32/magnify.exe", ProtectedKind::Input, false},
    {"system32/utilman.exe", ProtectedKind::Input, false},
    {"system32/atbroker.exe", ProtectedKind::Input, false},
    {"system32/imebroker.exe", ProtectedKind::Input, false},
    {"system32/ime/**/*.exe", ProtectedKind::Input, true},
    {"system32/driverstore/filerepository/**/*.exe", ProtectedKind::DriverPackage, true},
    {"systemapps/**/*.exe", ProtectedKind::ShellApp, true},
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
});

std::string_view file_name(std::string_view normalized) {
    const auto slash = normalized.rfind('/');
    return slash == std::string_view::npos ? normalized : normalized.substr(slash + 1);
}

}  // namespace

Protection Protection::windows_default(const std::string& system_root, const std::string& program_files) {
    Protection protection;
    const auto root = normalize_path(system_root);
    for (const auto& item : kSystemRoot) {
        protection.entries_.push_back(
            {root + "/" + std::string(item.relative), item.kind, item.signature_required});
    }
    protection.entries_.push_back(
        {normalize_path(program_files) + "/" + std::string(kTouchKeyboard), ProtectedKind::Input, false});
    return protection;
}

std::optional<ProtectedHit> Protection::classify(const ProcessFacts& facts) const {
    if (facts.kernel_critical) return ProtectedHit{ProtectedKind::KernelCritical, "kernel_critical", false};
    if (facts.protected_process)
        return ProtectedHit{ProtectedKind::ProtectedProcess, "protected_process", false};
    if (!facts.image_known || facts.remote_image) return std::nullopt;
    for (const auto& entry : entries_) {
        if (!path_matches(entry.pattern, facts.app.path)) continue;
        if (!entry.signature_required)
            return ProtectedHit{entry.kind, protected_kind_name(entry.kind), false};
        if (!facts.publisher_resolved) return ProtectedHit{entry.kind, protected_kind_name(entry.kind), true};
        if (!facts.app.publisher.empty())
            return ProtectedHit{entry.kind, protected_kind_name(entry.kind), false};
        return std::nullopt;
    }
    return std::nullopt;
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
