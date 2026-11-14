#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace houseguest::service {

class ProcessTrace {
public:
    using Handler = std::function<void(std::uint32_t pid, std::uint64_t event_ticks)>;

    explicit ProcessTrace(Handler handler);
    ~ProcessTrace();
    ProcessTrace(const ProcessTrace&) = delete;
    ProcessTrace& operator=(const ProcessTrace&) = delete;

    bool start(std::string& error);
    void stop();
    [[nodiscard]] bool alive() const { return alive_; }

    static void deliver(void* record);

private:
    Handler handler_;
    std::uint64_t session_ = 0;
    std::uint64_t consumer_ = 0;
    std::thread thread_;
    std::atomic<bool> alive_ = false;
};

}  // namespace houseguest::service
