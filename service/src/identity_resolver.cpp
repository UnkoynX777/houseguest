#include "identity_resolver.hpp"

#include <windows.h>

#include <algorithm>

#include "houseguest/system/crypto.hpp"
#include "houseguest/system/text.hpp"
#include "houseguest/system/windows_info.hpp"

namespace houseguest::service {
namespace {

constexpr std::size_t kMaxCacheEntries = 4096;

std::filesystem::path canonical_path(const std::filesystem::path& file) {
    std::wstring text = file.wstring();
    constexpr std::wstring_view kExtended = LR"(\\?\)";
    if (text.starts_with(kExtended) && text.size() > kExtended.size() + 1 &&
        text[kExtended.size() + 1] == L':')
        text.erase(0, kExtended.size());
    std::wstring longer(text.size() + 260, L'\0');
    const DWORD length = GetLongPathNameW(text.c_str(), longer.data(), static_cast<DWORD>(longer.size()));
    if (length == 0 || length >= longer.size()) return std::filesystem::path(text);
    longer.resize(length);
    return std::filesystem::path(longer);
}

std::string lowered(std::string value) {
    std::ranges::transform(value, value.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

}  // namespace

core::AppIdentity IdentityResolver::resolve(const std::filesystem::path& file, bool with_hash,
                                            bool with_publisher) {
    return resolve_checked(file, with_hash, with_publisher).identity;
}

Resolution IdentityResolver::resolve_checked(const std::filesystem::path& file, bool with_hash,
                                             bool with_publisher, const std::string& package_family) {
    Resolution result;
    auto& identity = result.identity;
    identity.path = system::path_to_utf8(canonical_path(file));
    identity.package_family = package_family;

    std::error_code ec;
    const auto size = std::filesystem::file_size(file, ec);
    const auto modified = std::filesystem::last_write_time(file, ec);
    const auto key = lowered(identity.path);

    std::scoped_lock lock(mutex_);
    if (cache_.size() > kMaxCacheEntries) cache_.clear();
    auto& entry = cache_[key];
    if (entry.size != size || entry.modified != modified) entry = Entry{size, modified, {}, {}};
    if (with_hash && !entry.sha256) entry.sha256 = system::sha256_file(file);
    if (with_publisher && !entry.publisher) entry.publisher = system::trusted_publisher(file);
    result.complete = !with_hash || entry.sha256.has_value();
    identity.sha256 = entry.sha256.value_or("");
    identity.publisher = entry.publisher.value_or("");
    return result;
}

std::size_t IdentityResolver::cached() const {
    std::scoped_lock lock(mutex_);
    return cache_.size();
}

}  // namespace houseguest::service
