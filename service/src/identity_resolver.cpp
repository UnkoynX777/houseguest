#include "identity_resolver.hpp"

#include <algorithm>

#include "houseguest/system/crypto.hpp"
#include "houseguest/system/text.hpp"
#include "houseguest/system/windows_info.hpp"

namespace houseguest::service {
namespace {

constexpr std::size_t kMaxCacheEntries = 4096;

std::string lowered(std::string value) {
    std::ranges::transform(value, value.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

}  // namespace

core::AppIdentity IdentityResolver::resolve(const std::filesystem::path& file, bool with_hash,
                                            bool with_publisher) {
    core::AppIdentity identity;
    identity.path = system::path_to_utf8(file);

    std::error_code ec;
    const auto size = std::filesystem::file_size(file, ec);
    const auto modified = std::filesystem::last_write_time(file, ec);
    const auto key = lowered(identity.path);

    std::scoped_lock lock(mutex_);
    if (cache_.size() > kMaxCacheEntries) cache_.clear();
    auto& entry = cache_[key];
    if (entry.size != size || entry.modified != modified) entry = Entry{size, modified, {}, {}};
    if (with_hash && !entry.sha256) entry.sha256 = system::sha256_file(file).value_or("");
    if (with_publisher && !entry.publisher) entry.publisher = system::trusted_publisher(file);
    identity.sha256 = entry.sha256.value_or("");
    identity.publisher = entry.publisher.value_or("");
    return identity;
}

std::size_t IdentityResolver::cached() const {
    std::scoped_lock lock(mutex_);
    return cache_.size();
}

}  // namespace houseguest::service
