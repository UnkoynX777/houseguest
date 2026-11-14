#include "service_monitor.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>

#include "houseguest/system/pipe.hpp"

namespace houseguest::host {
namespace {

using core::Json;

constexpr auto kPollInterval = std::chrono::seconds(3);

ServiceStatus query() {
    ServiceStatus status;
    system::PipeClient client;
    const auto reply = client.call({{"id", 1}, {"cmd", "status"}});
    if (!reply || !reply->value("ok", false) || !reply->contains("data")) return status;
    const Json& data = (*reply)["data"];
    status.reachable = true;
    status.configured = data.value("configured", false);
    status.armed = data.value("armed", false);
    status.enforcement_available = data.value("enforcement", std::string()) == "supervised";
    status.store_tampered = data.value("store_tampered", false);
    status.ends_at = data.value("ends_at", std::int64_t{0});
    status.server_now = data.value("now", std::int64_t{0});
    status.profile = data.value("profile", std::string());
    return status;
}

}  // namespace

ServiceMonitor::ServiceMonitor(HWND target, UINT message)
    : worker_([target, message](const std::stop_token& stop) {
          std::mutex mutex;
          std::condition_variable_any wake;
          std::unique_lock lock(mutex);
          while (!stop.stop_requested()) {
              auto* status = new ServiceStatus(query());
              if (!PostMessageW(target, message, 0, reinterpret_cast<LPARAM>(status))) delete status;
              wake.wait_for(lock, stop, kPollInterval, [] { return false; });
          }
      }) {}

ServiceMonitor::~ServiceMonitor() = default;

}  // namespace houseguest::host
