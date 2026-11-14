#pragma once

#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "houseguest/core/serialization.hpp"

namespace houseguest::service {

// Establishes which processes are Houseguest itself. A process counts only when its file is one of the
// installed executables whose hash was pinned when the service started, from a folder the restricted
// accounts cannot write to. The WebView2 processes the interface needs count only when they run the
// Microsoft-signed runtime, from a folder those accounts cannot write to, below a verified Houseguest
// process.
class ContinuityGuard {
public:
    explicit ContinuityGuard(std::filesystem::path install_dir);

    void pin();
    void restrict_to(std::vector<std::string> guest_sids);

    [[nodiscard]] bool is_houseguest_image(const std::filesystem::path& image) const;
    [[nodiscard]] static bool is_webview_image(const std::filesystem::path& image);
    [[nodiscard]] bool houseguest_verified(const std::filesystem::path& image, const std::string& sha256);
    [[nodiscard]] bool webview_verified(const std::filesystem::path& image, const std::string& publisher);

    struct Binary {
        std::string path;
        std::string sha256;
    };
    struct Report {
        std::vector<Binary> verified;
        bool install_trusted = false;
        bool pinned = false;
        std::vector<std::string> missing;
        std::vector<std::string> webview_runtimes;
        bool webview_trusted = false;
    };
    [[nodiscard]] Report report(const std::vector<std::string>& guest_sids) const;
    [[nodiscard]] core::Json report_json(const std::vector<std::string>& guest_sids) const;

    static constexpr const char* kPublisherPrefix = "CN=Microsoft Corporation";

private:
    bool trusted_directory(const std::filesystem::path& directory);
    [[nodiscard]] std::vector<std::filesystem::path> runtime_directories() const;

    std::filesystem::path install_dir_;
    std::map<std::string, std::string> pinned_;
    mutable std::mutex mutex_;
    std::vector<std::string> guests_;
    std::map<std::string, bool> trusted_;
};

}  // namespace houseguest::service
