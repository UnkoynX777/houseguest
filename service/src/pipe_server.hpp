#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "controller.hpp"

namespace houseguest::service {

class PipeServer {
public:
    explicit PipeServer(Controller& controller);
    ~PipeServer();

    bool start(std::string& error);
    void stop();
    [[nodiscard]] core::Json stats() const;

private:
    void accept_loop();
    void serve(void* pipe, std::string peer);
    bool admit(const std::string& peer);
    void release(const std::string& peer, bool was_pending);
    void leave_pending(const std::string& peer);

    Controller& controller_;
    std::thread acceptor_;
    std::atomic<bool> stopping_ = false;
    void* stop_event_ = nullptr;
    void* first_instance_ = nullptr;

    mutable std::mutex mutex_;
    std::condition_variable idle_;
    struct Usage {
        int active = 0;
        int pending = 0;
    };
    std::unordered_map<std::string, Usage> per_user_;
    int active_ = 0;
    int pending_ = 0;

    std::atomic<std::uint64_t> accepted_ = 0;
    std::atomic<std::uint64_t> rejected_busy_ = 0;
    std::atomic<std::uint64_t> timed_out_ = 0;
    std::atomic<std::uint64_t> malformed_ = 0;
    std::atomic<std::uint64_t> rate_limited_ = 0;
    std::atomic<std::uint64_t> oversized_ = 0;
};

}  // namespace houseguest::service
