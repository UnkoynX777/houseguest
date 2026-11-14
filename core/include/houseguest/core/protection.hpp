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
};

struct ProtectedHit {
    ProtectedKind kind;
    std::string_view reason;
    bool needs_publisher = false;
};

// Components that Windows needs to log in, draw the desktop, accept input and recover. The table is compiled
// in and cannot be edited at run time. A process is protected by what the kernel says about it first, then by
// an exact location under a directory only administrators can write. Directory-wide grants (driver packages,
// system apps) additionally require a valid signature. Nothing is protected by file name alone, and System32
// as a whole is never protected. Houseguest's own processes are handled separately as continuity.
class Protection {
public:
    [[nodiscard]] static Protection windows_default(const std::string& system_root,
                                                    const std::string& program_files);

    [[nodiscard]] std::optional<ProtectedHit> classify(const ProcessFacts& facts) const;
    [[nodiscard]] bool covers(std::string_view path) const;
    [[nodiscard]] std::vector<std::string> patterns() const;

private:
    struct Entry {
        std::string pattern;
        ProtectedKind kind;
        bool signature_required;
    };
    std::vector<Entry> entries_;
};

[[nodiscard]] std::string_view protected_kind_name(ProtectedKind kind);

// Tools that run arbitrary code or change the machine. Never inherited through application membership.
[[nodiscard]] bool is_sensitive_tool(std::string_view path);

[[nodiscard]] std::span<const std::string_view> sensitive_tool_names();

}  // namespace houseguest::core
