#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <thread>

namespace houseguest::host {

struct ServiceStatus {
    bool reachable = false;
    bool configured = false;
    bool armed = false;
    bool enforcement_available = false;
    bool store_tampered = false;
    std::int64_t ends_at = 0;
    std::int64_t server_now = 0;
    std::string profile;
};

class ServiceMonitor {
public:
    ServiceMonitor(HWND target, UINT message);
    ~ServiceMonitor();
    ServiceMonitor(const ServiceMonitor&) = delete;
    ServiceMonitor& operator=(const ServiceMonitor&) = delete;

private:
    std::jthread worker_;
};

}  // namespace houseguest::host
