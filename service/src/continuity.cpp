#include "continuity.hpp"

#include <windows.h>

#include <algorithm>
#include <array>

#include "houseguest/core/glob.hpp"
#include "houseguest/core/subject.hpp"
#include "houseguest/system/crypto.hpp"
#include "houseguest/system/text.hpp"
#include "houseguest/system/windows_info.hpp"

namespace houseguest::service {
namespace {

constexpr std::array<std::string_view, 3> kExecutables = {"houseguest.exe", "houseguest-cli.exe",
                                                          "houseguest-service.exe"};
constexpr std::string_view kWebViewName = "msedgewebview2.exe";
constexpr std::string_view kWebViewFolder = "/microsoft/edgewebview/application/";

std::string lowered(const std::filesystem::path& path) {
    return core::normalize_path(system::path_to_utf8(path));
}

}  // namespace

ContinuityGuard::ContinuityGuard(std::filesystem::path install_dir) : install_dir_(std::move(install_dir)) {}

void ContinuityGuard::pin() {
    std::map<std::string, std::string> fresh;
    for (const auto name : kExecutables) {
        const auto file = install_dir_ / system::path_from_utf8(name);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(file, ec)) continue;
        if (const auto hash = system::sha256_file(file)) fresh[std::string(name)] = *hash;
    }
    std::scoped_lock lock(mutex_);
    pinned_ = std::move(fresh);
}

void ContinuityGuard::restrict_to(std::vector<std::string> guest_sids) {
    std::scoped_lock lock(mutex_);
    guests_ = std::move(guest_sids);
    trusted_.clear();
}

bool ContinuityGuard::is_houseguest_image(const std::filesystem::path& image) const {
    const auto path = lowered(image);
    const auto folder = lowered(install_dir_) + "/";
    if (!path.starts_with(folder)) return false;
    const auto name = path.substr(folder.size());
    return std::ranges::find(kExecutables, name) != kExecutables.end();
}

bool ContinuityGuard::is_webview_image(const std::filesystem::path& image) {
    const auto path = lowered(image);
    return path.ends_with(std::string("/") + std::string(kWebViewName)) &&
           path.find(kWebViewFolder) != std::string::npos;
}

bool ContinuityGuard::trusted_directory(const std::filesystem::path& directory) {
    const auto key = lowered(directory);
    std::vector<std::string> guests;
    {
        std::scoped_lock lock(mutex_);
        if (const auto it = trusted_.find(key); it != trusted_.end()) return it->second;
        guests = guests_;
    }
    const bool trusted = !system::directory_writable_by(directory, guests);
    std::scoped_lock lock(mutex_);
    trusted_[key] = trusted;
    return trusted;
}

bool ContinuityGuard::houseguest_verified(const std::filesystem::path& image, const std::string& sha256) {
    if (sha256.empty() || !is_houseguest_image(image)) return false;
    const auto name = lowered(image.filename());
    {
        std::scoped_lock lock(mutex_);
        const auto it = pinned_.find(name);
        if (it == pinned_.end() || it->second != sha256) return false;
    }
    return trusted_directory(install_dir_);
}

bool ContinuityGuard::webview_verified(const std::filesystem::path& image, const std::string& publisher) {
    if (!is_webview_image(image) || !core::subject_has(publisher, "CN", "Microsoft Corporation") ||
        !core::subject_has(publisher, "O", "Microsoft Corporation"))
        return false;
    return trusted_directory(image.parent_path());
}

std::vector<std::filesystem::path> ContinuityGuard::runtime_directories() const {
    std::vector<std::filesystem::path> found;
    for (const auto* variable : {L"ProgramFiles(x86)", L"ProgramFiles"}) {
        std::wstring root(MAX_PATH, L'\0');
        root.resize(GetEnvironmentVariableW(variable, root.data(), static_cast<DWORD>(root.size())));
        if (root.empty()) continue;
        const std::filesystem::path application =
            std::filesystem::path(root) / L"Microsoft" / L"EdgeWebView" / L"Application";
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(application, ec)) {
            if (entry.is_directory(ec) && std::filesystem::exists(entry.path() / L"msedgewebview2.exe", ec))
                found.push_back(entry.path());
        }
    }
    return found;
}

ContinuityGuard::Report ContinuityGuard::report(const std::vector<std::string>& guest_sids) const {
    Report report;
    report.install_trusted = !system::directory_writable_by(install_dir_, guest_sids);
    {
        std::scoped_lock lock(mutex_);
        report.pinned = pinned_.contains("houseguest-service.exe");
        for (const auto name : kExecutables) {
            const auto it = pinned_.find(std::string(name));
            if (it == pinned_.end()) {
                report.missing.emplace_back(name);
            } else if (report.install_trusted) {
                report.verified.push_back(
                    {system::path_to_utf8(install_dir_ / system::path_from_utf8(name)), it->second});
            }
        }
    }
    report.webview_trusted = true;
    for (const auto& directory : runtime_directories()) {
        report.webview_runtimes.push_back(system::path_to_utf8(directory));
        report.webview_trusted =
            report.webview_trusted && !system::directory_writable_by(directory, guest_sids);
    }
    return report;
}

core::Json ContinuityGuard::report_json(const std::vector<std::string>& guest_sids) const {
    const auto r = report(guest_sids);
    return {{"install_directory", system::path_to_utf8(install_dir_)},
            {"install_trusted", r.install_trusted},
            {"pinned", r.pinned},
            {"missing", r.missing},
            {"webview_runtimes", r.webview_runtimes},
            {"webview_trusted", r.webview_trusted}};
}

}  // namespace houseguest::service
