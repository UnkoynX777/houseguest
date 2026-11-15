#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "houseguest/core/serialization.hpp"

namespace houseguest::service::cloudflare {

enum class TunnelState { Stopped, Starting, Connected, Reconnecting, Failed };
[[nodiscard]] const char* state_name(TunnelState state);

enum class LogKind { Info, Connected, Disconnected, Error };
struct LogEvent {
    LogKind kind = LogKind::Info;
    std::string text;
};
// Reads one line of cloudflared output. Anything that looks like a credential is removed from the text.
[[nodiscard]] LogEvent classify_log_line(const std::string& line);

// Restart delays grow 1, 2, 4 ... seconds up to five minutes, restart after a stable run begins again from one
// second, and more than six restarts in ten minutes stops the supervisor instead of spinning.
class Backoff {
public:
    using Clock = std::chrono::steady_clock;
    struct Decision {
        bool give_up = false;
        std::chrono::seconds delay{0};
    };
    Decision on_exit(Clock::time_point started, Clock::time_point exited);
    void reset();

    static constexpr int kMaxRestartsPerWindow = 6;
    static constexpr auto kWindow = std::chrono::minutes(10);
    static constexpr auto kStable = std::chrono::seconds(60);
    static constexpr auto kMaxDelay = std::chrono::seconds(300);

private:
    int streak_ = 0;
    std::deque<Clock::time_point> restarts_;
};

struct LaunchSpec {
    std::filesystem::path executable;
    std::vector<std::string> arguments;
    std::filesystem::path directory;
};

class RunningProcess {
public:
    virtual ~RunningProcess() = default;
    virtual bool alive() = 0;
    virtual void stop() = 0;
    virtual void wait(std::chrono::milliseconds limit) = 0;
};
using LineSink = std::function<void(const std::string&)>;
using Launcher = std::function<std::unique_ptr<RunningProcess>(const LaunchSpec&, LineSink)>;

struct TunnelSettings {
    std::string tunnel_id;
    std::string account_id;
    std::string secret_base64;
    std::string hostname;
};

[[nodiscard]] std::string render_config(const TunnelSettings& settings, std::uint16_t local_port,
                                        const std::filesystem::path& credentials);
[[nodiscard]] std::string render_credentials(const TunnelSettings& settings);

// Runs one cloudflared process at a time and keeps it connected. The configuration and the credentials file exist
// only while the tunnel runs and are removed when it stops. Nothing else in the service depends on this class.
class TunnelManager {
public:
    TunnelManager(Launcher launcher, std::filesystem::path work_directory);
    ~TunnelManager();

    bool start(const std::filesystem::path& executable, const TunnelSettings& settings, std::uint16_t local_port,
               std::string& error);
    void stop();

    [[nodiscard]] TunnelState state() const;
    [[nodiscard]] core::Json status() const;
    [[nodiscard]] std::vector<std::string> logs() const;

    // Milliseconds that stand for one second of restart delay; tests shrink it.
    void set_delay_unit(std::chrono::milliseconds unit) { unit_ = unit; }

private:
    void supervise(std::stop_token stop);
    void note(LogKind kind, const std::string& text);
    void remove_files();

    Launcher launcher_;
    std::filesystem::path directory_;
    LaunchSpec spec_;
    TunnelSettings settings_;
    std::uint16_t port_ = 0;
    std::chrono::milliseconds unit_{1000};

    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    TunnelState state_ = TunnelState::Stopped;
    std::int64_t connected_since_ = 0;
    std::int64_t last_connected_ = 0;
    std::int64_t last_exit_ = 0;
    int restarts_ = 0;
    int connections_ = 0;
    std::deque<std::string> logs_;
    std::string last_error_;
    std::unique_ptr<RunningProcess> process_;
    std::jthread thread_;
};

}  // namespace houseguest::service::cloudflare
