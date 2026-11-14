#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "houseguest/core/types.hpp"

namespace houseguest::core {

enum class ProtectedKind {
    KernelCritical,
    ProtectedProcess,
    WindowsCore,
    WindowsShell,
    Input,
    DriverPackage,
    ShellApp,
    Infrastructure,
    ComServer,
};

struct ProtectedHit {
    ProtectedKind kind;
    std::string_view reason;
    bool needs_publisher = false;
};

// Components that Windows needs to log in, draw the desktop, accept input and recover. The table is compiled
// in and cannot be edited at run time. A process is protected by what the kernel says about it first, then by
// an exact location under a directory only administrators can write. Directory-wide grants (driver packages,
// system apps) and system infrastructure additionally require a valid signature, and infrastructure requires
// the Windows signing identity, not just any Microsoft certificate. Nothing is protected by file name alone,
// and System32 as a whole is never protected.
//
// One further class is recognised by evidence instead of by name: a Windows-signed program in System32 or
// SysWOW64 that an administrator registered as a COM local server and that the DCOM launcher started. It is
// never a tool that runs arbitrary code, and a program that only sits in System32 does not qualify.
// Houseguest's own processes are handled separately as continuity.
class Protection {
public:
    enum class Signer { None, Any, Windows };

    [[nodiscard]] static Protection windows_default(const std::string& system_root,
                                                    const std::string& program_files);

    [[nodiscard]] std::optional<ProtectedHit> classify(const ProcessFacts& facts) const;
    [[nodiscard]] bool covers(std::string_view path) const;
    [[nodiscard]] bool may_be_com_server(std::string_view path) const;
    [[nodiscard]] std::vector<std::string> patterns() const;

private:
    struct Entry {
        std::string pattern;
        ProtectedKind kind;
        Signer signer;
    };
    [[nodiscard]] std::optional<ProtectedHit> classify_by_evidence(const ProcessFacts& facts) const;

    std::vector<Entry> entries_;
    std::vector<std::string> com_areas_;
    std::string dcom_launcher_;
};

[[nodiscard]] std::string_view protected_kind_name(ProtectedKind kind);

// True when the signer is the Windows operating system itself ("Microsoft Windows"), which is a different
// identity from the Microsoft Corporation certificates that sign Office, Visual Studio or Edge.
[[nodiscard]] bool is_windows_signer(std::string_view publisher);

// Tools that run arbitrary code or change the machine. Never inherited through application membership.
[[nodiscard]] bool is_sensitive_tool(std::string_view path);

[[nodiscard]] std::span<const std::string_view> sensitive_tool_names();

}  // namespace houseguest::core
