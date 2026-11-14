#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <set>
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

private:
    void accept_loop();
    void serve(void* pipe);

    Controller& controller_;
    std::thread acceptor_;
    std::atomic<bool> stopping_ = false;
    std::mutex mutex_;
    std::condition_variable idle_;
    std::set<void*> connections_;
    std::unordered_map<std::string, int> per_user_;
    void* first_instance_ = nullptr;
};

}  // namespace houseguest::service
