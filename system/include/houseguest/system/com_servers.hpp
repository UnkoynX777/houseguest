#pragma once

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>

namespace houseguest::system {

// The programs an administrator registered as COM local servers (HKLM CLSID \ LocalServer32, both registry
// views). Only machine-wide registrations count: a restricted account cannot write them. The index is built
// on first use and rebuilt on the next lookup after Windows reports a change below the CLSID keys, so a
// registration added or removed is seen without polling and without a thread.
class ComServerIndex {
public:
    ComServerIndex();
    ~ComServerIndex();
    ComServerIndex(const ComServerIndex&) = delete;
    ComServerIndex& operator=(const ComServerIndex&) = delete;

    [[nodiscard]] bool registered(const std::filesystem::path& image);
    [[nodiscard]] std::size_t size();
    [[nodiscard]] std::size_t rebuilds() const;

    // The program a LocalServer32 command line starts, expanded but not canonicalised.
    [[nodiscard]] static std::filesystem::path executable_of(std::wstring command);

private:
    struct Watch;
    void refresh_locked();

    std::mutex mutex_;
    std::unordered_set<std::string> servers_;
    std::unique_ptr<Watch> watch_;
    bool built_ = false;
    std::atomic<std::size_t> rebuilds_ = 0;
};

}  // namespace houseguest::system
