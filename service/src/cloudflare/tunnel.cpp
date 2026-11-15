#include "tunnel.hpp"

#include <algorithm>
#include <fstream>
#include <regex>

#include "../command_support.hpp"
#include "api.hpp"

namespace houseguest::service::cloudflare {
namespace {

using core::Json;
constexpr std::size_t kMaxLogs = 200;

std::string redact(std::string line) {
    static const std::regex secret(R"([A-Za-z0-9+/=_-]{40,})");
    line = std::regex_replace(line, secret, "[hidden]");
    if (line.size() > 400) line.resize(400);
    return line;
}

bool contains(const std::string& text, const char* needle) {
    return text.find(needle) != std::string::npos;
}

}  // namespace

const char* state_name(TunnelState state) {
    switch (state) {
        case TunnelState::Stopped:
            return "stopped";
        case TunnelState::Starting:
            return "starting";
        case TunnelState::Connected:
            return "connected";
        case TunnelState::Reconnecting:
            return "reconnecting";
        case TunnelState::Failed:
            return "failed";
    }
    return "stopped";
}

LogEvent classify_log_line(const std::string& raw) {
    LogEvent event;
    event.text = redact(raw);
    if (contains(raw, "Registered tunnel connection"))
        event.kind = LogKind::Connected;
    else if (contains(raw, "Unregistered tunnel connection") || contains(raw, "Lost connection") ||
             contains(raw, "Retrying connection"))
        event.kind = LogKind::Disconnected;
    else if (contains(raw, " ERR ") || contains(raw, "level=error") || contains(raw, "failed to") ||
             contains(raw, "Failed to"))
        event.kind = LogKind::Error;
    return event;
}

Backoff::Decision Backoff::on_exit(Clock::time_point started, Clock::time_point exited) {
    if (exited - started >= kStable) {
        streak_ = 0;
        restarts_.clear();
    }
    while (!restarts_.empty() && exited - restarts_.front() > kWindow) restarts_.pop_front();
    restarts_.push_back(exited);
    if (static_cast<int>(restarts_.size()) > kMaxRestartsPerWindow) return {true, {}};
    const auto seconds = std::min<long long>(kMaxDelay.count(), 1LL << std::min(streak_, 9));
    ++streak_;
    return {false, std::chrono::seconds(seconds)};
}

void Backoff::reset() {
    streak_ = 0;
    restarts_.clear();
}

std::string render_credentials(const TunnelSettings& settings) {
    return Json{{"AccountTag", settings.account_id},
                {"TunnelSecret", settings.secret_base64},
                {"TunnelID", settings.tunnel_id}}
        .dump();
}

std::string render_config(const TunnelSettings& settings, std::uint16_t local_port,
                          const std::filesystem::path& credentials) {
    if (!valid_hostname(settings.hostname)) throw std::invalid_argument("The hostname is not valid.");
    static const std::regex id(R"(^[0-9A-Za-z-]{8,64}$)");
    if (!std::regex_match(settings.tunnel_id, id)) throw std::invalid_argument("The tunnel id is not valid.");
    const auto file = credentials.generic_string();
    if (file.find('"') != std::string::npos || file.find('\n') != std::string::npos)
        throw std::invalid_argument("The credentials path is not valid.");
    return "tunnel: " + settings.tunnel_id + "\ncredentials-file: \"" + file +
           "\"\nno-autoupdate: true\ningress:\n  - hostname: " + settings.hostname +
           "\n    service: http://127.0.0.1:" + std::to_string(local_port) +
           "\n    originRequest:\n      httpHostHeader: " + settings.hostname +
           "\n  - service: http_status:404\n";
}

TunnelManager::TunnelManager(Launcher launcher, std::filesystem::path work_directory)
    : launcher_(std::move(launcher)), directory_(std::move(work_directory)) {}

TunnelManager::~TunnelManager() {
    stop();
}

TunnelState TunnelManager::state() const {
    std::scoped_lock lock(mutex_);
    return state_;
}

void TunnelManager::note(LogKind kind, const std::string& text) {
    std::scoped_lock lock(mutex_);
    if (kind == LogKind::Connected) {
        ++connections_;
        state_ = TunnelState::Connected;
        connected_since_ = connected_since_ ? connected_since_ : now_seconds();
        last_connected_ = now_seconds();
    } else if (kind == LogKind::Disconnected) {
        connections_ = std::max(0, connections_ - 1);
        if (connections_ == 0 && state_ == TunnelState::Connected) {
            state_ = TunnelState::Reconnecting;
            connected_since_ = 0;
        }
    } else if (kind == LogKind::Error) {
        last_error_ = text;
    }
    logs_.push_back(text);
    while (logs_.size() > kMaxLogs) logs_.pop_front();
}

void TunnelManager::remove_files() {
    std::error_code ignored;
    std::filesystem::remove(directory_ / L"credentials.json", ignored);
    std::filesystem::remove(directory_ / L"config.yml", ignored);
}

bool TunnelManager::start(const std::filesystem::path& executable, const TunnelSettings& settings,
                          std::uint16_t local_port, std::string& error) {
    {
        std::scoped_lock lock(mutex_);
        if (state_ != TunnelState::Stopped && state_ != TunnelState::Failed) {
            error = "The tunnel is already running.";
            return false;
        }
    }
    stop();
    try {
        std::filesystem::create_directories(directory_);
        const auto credentials = directory_ / L"credentials.json";
        const auto config = directory_ / L"config.yml";
        const auto config_text = render_config(settings, local_port, credentials);
        std::ofstream(credentials, std::ios::binary | std::ios::trunc) << render_credentials(settings);
        std::ofstream(config, std::ios::binary | std::ios::trunc) << config_text;
        spec_ = {executable, {"--no-autoupdate", "tunnel", "--config", config.string(), "run"}, directory_};
    } catch (const std::exception& e) {
        remove_files();
        error = e.what();
        return false;
    }
    settings_ = settings;
    port_ = local_port;
    {
        std::scoped_lock lock(mutex_);
        state_ = TunnelState::Starting;
        restarts_ = 0;
        connections_ = 0;
        connected_since_ = 0;
        last_error_.clear();
    }
    thread_ = std::jthread([this](const std::stop_token& token) { supervise(token); });
    return true;
}

void TunnelManager::stop() {
    if (thread_.joinable()) {
        thread_.request_stop();
        wake_.notify_all();
        thread_.join();
    }
    remove_files();
    std::scoped_lock lock(mutex_);
    process_.reset();
    if (state_ != TunnelState::Failed) state_ = TunnelState::Stopped;
    connected_since_ = 0;
    connections_ = 0;
}

void TunnelManager::supervise(const std::stop_token& stop) {
    Backoff backoff;
    while (!stop.stop_requested()) {
        const auto started = Backoff::Clock::now();
        std::unique_ptr<RunningProcess> process;
        try {
            process = launcher_(spec_, [this](const std::string& line) {
                const auto event = classify_log_line(line);
                note(event.kind, event.text);
            });
        } catch (const std::exception& e) {
            note(LogKind::Error, std::string("Could not start cloudflared: ") + e.what());
        }
        if (process) {
            RunningProcess* raw = process.get();
            {
                std::scoped_lock lock(mutex_);
                process_ = std::move(process);
            }
            while (!stop.stop_requested() && raw->alive()) raw->wait(std::chrono::milliseconds(200));
            std::unique_ptr<RunningProcess> finished;
            {
                std::scoped_lock lock(mutex_);
                finished = std::move(process_);
            }
            if (finished) finished->stop();
        }
        if (stop.stop_requested()) break;

        const auto decision = backoff.on_exit(started, Backoff::Clock::now());
        std::unique_lock lock(mutex_);
        last_exit_ = now_seconds();
        connected_since_ = 0;
        connections_ = 0;
        if (decision.give_up) {
            state_ = TunnelState::Failed;
            last_error_ =
                "cloudflared kept stopping, so the tunnel was turned off. Check the log and start it again.";
            logs_.push_back(last_error_);
            return;
        }
        state_ = TunnelState::Reconnecting;
        ++restarts_;
        wake_.wait_for(lock, stop, std::chrono::milliseconds(decision.delay.count() * unit_.count()),
                       [] { return false; });
    }
}

Json TunnelManager::status() const {
    std::scoped_lock lock(mutex_);
    const auto now = now_seconds();
    return Json{{"state", state_name(state_)},
                {"hostname", settings_.hostname},
                {"tunnel_id", settings_.tunnel_id},
                {"connected_seconds", connected_since_ ? now - connected_since_ : 0},
                {"last_connected", last_connected_},
                {"last_exit", last_exit_},
                {"restarts", restarts_},
                {"last_error", last_error_}};
}

std::vector<std::string> TunnelManager::logs() const {
    std::scoped_lock lock(mutex_);
    return {logs_.begin(), logs_.end()};
}

}  // namespace houseguest::service::cloudflare
