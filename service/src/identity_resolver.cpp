#include "identity_resolver.hpp"

#include <windows.h>

#include <algorithm>

#include "houseguest/system/crypto.hpp"
#include "houseguest/system/text.hpp"
#include "houseguest/system/windows_info.hpp"

namespace houseguest::service {
namespace {

constexpr std::size_t kMaxCacheEntries = 4096;

std::filesystem::path without_extended_prefix(const std::filesystem::path& file) {
    std::wstring text = file.wstring();
    constexpr std::wstring_view kExtended = LR"(\\?\)";
    if (text.starts_with(kExtended) && text.size() > kExtended.size() + 1 &&
        text[kExtended.size() + 1] == L':')
        text.erase(0, kExtended.size());
    return std::filesystem::path(text);
}

std::filesystem::path canonical_path(const std::filesystem::path& file) {
    const std::wstring text = file.wstring();
    std::wstring longer(text.size() + 260, L'\0');
    const DWORD length = GetLongPathNameW(text.c_str(), longer.data(), static_cast<DWORD>(longer.size()));
    if (length == 0 || length >= longer.size()) return file;
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
    identity.package_family = package_family;

    const auto stripped = without_extended_prefix(file);
    if (!system::is_local_disk_path(stripped)) {
        identity.path = system::path_to_utf8(stripped);
        result.remote = true;
        result.complete = !with_hash;
        return result;
    }
    const auto canonical = canonical_path(stripped);
    identity.path = system::path_to_utf8(canonical);

    const auto stamp = system::file_stamp(canonical);
    const auto key = lowered(identity.path);

    Entry known;
    {
        std::scoped_lock lock(mutex_);
        if (cache_.size() > kMaxCacheEntries) cache_.clear();
        auto& entry = cache_[key];
        if (!stamp || entry.stamp != stamp) entry = Entry{stamp, {}, {}};
        known = entry;
    }

    if (with_hash && !known.sha256) known.sha256 = system::sha256_file(canonical);
    if (with_publisher && !known.publisher) known.publisher = system::trusted_publisher(canonical);

    {
        std::scoped_lock lock(mutex_);
        const auto it = cache_.find(key);
        if (stamp && it != cache_.end() && it->second.stamp == stamp) {
            if (known.sha256) it->second.sha256 = known.sha256;
            if (known.publisher) it->second.publisher = known.publisher;
        }
    }

    result.complete = !with_hash || known.sha256.has_value();
    identity.sha256 = known.sha256.value_or("");
    identity.publisher = known.publisher.value_or("");
    return result;
}

std::size_t IdentityResolver::cached() const {
    std::scoped_lock lock(mutex_);
    return cache_.size();
}

}  // namespace houseguest::service
