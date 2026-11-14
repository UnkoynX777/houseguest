#pragma once

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <list>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace houseguest::host {

// Extracts the icon Windows shows for an executable and returns it as a PNG data URL. Extraction runs on one
// worker thread that owns its COM apartment, results are cached in memory and in the user's local app data
// keyed by path, size and modification time, so a changed file gets a fresh icon and the window never waits.
class IconService {
public:
    using Reply = std::function<void(std::string data_url)>;

    explicit IconService(std::filesystem::path cache_directory);
    ~IconService();
    IconService(const IconService&) = delete;
    IconService& operator=(const IconService&) = delete;

    void request(const std::string& utf8_path, int pixels, Reply reply);

    static constexpr std::size_t kMemoryEntries = 600;
    static constexpr std::size_t kQueueLimit = 400;

private:
    struct Job {
        std::string path;
        int pixels;
        Reply reply;
    };

    std::string produce(const std::string& utf8_path, int pixels);
    std::string cached(const std::string& key);
    void remember(const std::string& key, const std::string& value);
    void run(const std::stop_token& stop);

    std::filesystem::path directory_;
    std::mutex mutex_;
    std::condition_variable_any ready_;
    std::deque<Job> queue_;
    std::list<std::string> order_;
    std::unordered_map<std::string, std::pair<std::string, std::list<std::string>::iterator>> memory_;
    std::jthread worker_;
};

}  // namespace houseguest::host
