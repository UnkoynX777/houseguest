#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace houseguest::service {

// Tells the service when something that feeds the application library has changed, so the library is refreshed
// when it needs to be and not on a timer. It watches a fixed, small set of places: the Start menu and desktop
// folders, the top level of every program folder and fixed volume (new install folders), packaged apps, Steam
// library manifests, folders registered as portable, and the Uninstall registry keys. Nothing is watched
// recursively except the Start menu, and a burst of changes is reported once after it settles. The set is
// recomputed periodically, which also notices volumes that appear or go away.
class ChangeWatcher {
public:
    using Roots = std::function<std::vector<std::filesystem::path>()>;
    using Callback = std::function<void(const std::string& reason)>;

    ChangeWatcher(Roots extra_roots, Callback on_change);
    ~ChangeWatcher();
    ChangeWatcher(const ChangeWatcher&) = delete;
    ChangeWatcher& operator=(const ChangeWatcher&) = delete;

    void start();
    void stop();
    void refresh_places() { recompute_ = true; }

    [[nodiscard]] std::uint64_t signals() const { return signals_.load(); }
    [[nodiscard]] std::uint64_t reports() const { return reports_.load(); }
    [[nodiscard]] std::size_t watching() const { return watching_.load(); }

    static constexpr auto kQuiet = std::chrono::seconds(4);
    static constexpr auto kLongest = std::chrono::seconds(30);
    static constexpr auto kRecompute = std::chrono::seconds(30);

private:
    void run(const std::stop_token& stop);

    Roots extra_roots_;
    Callback on_change_;
    std::jthread thread_;
    std::atomic<std::uint64_t> signals_{0};
    std::atomic<std::uint64_t> reports_{0};
    std::atomic<std::size_t> watching_{0};
    std::atomic<bool> recompute_{false};
};

}  // namespace houseguest::service
