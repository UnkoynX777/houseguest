#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "houseguest/system/capabilities.hpp"

#include "audit.hpp"
#include "store.hpp"
#include "supervisor.hpp"

namespace houseguest::service {

struct Caller {
    std::string sid;
    bool elevated = false;
};

class Controller {
public:
    Controller(std::filesystem::path data_directory, bool enable_enforcement);
    ~Controller();

    core::Json handle(const Caller& caller, const core::Json& request);
    void shutdown();
    void set_ipc_stats(std::function<core::Json()> provider);

private:
    enum class Access { Anyone, Elevated, Owner };
    struct Command {
        Access access;
        bool unlocked;
        std::function<core::Json(const Caller&, const core::Json&)> run;
    };
    struct Token {
        std::string sid;
        std::chrono::steady_clock::time_point expires;
    };
    struct Compiled {
        std::shared_ptr<EnforcementPolicy> policy;
        core::Json findings = core::Json::array();
        bool has_errors = false;
        bool has_warnings = false;
    };

    void register_commands();
    void register_account_commands();
    void register_policy_commands();
    void register_session_commands();
    void register_system_commands();
    void add(const char* name, Access access, std::function<core::Json(const Caller&, const core::Json&)> run,
             bool unlocked = false);
    void require_owner(const Caller& caller, const core::Json& request);
    std::string verify_pin_and_issue_token(const Caller& caller, const std::string& pin);
    void verify_pin(const std::string& pin);
    void set_pin(const std::string& pin);
    std::string issue_token(const std::string& sid);

    Compiled compile(const core::Profile& profile, const std::vector<core::Rule>& exceptions,
                     const std::vector<std::string>& guests, bool audit_only) const;
    void arm(const Session& session);
    void end_session(const std::string& reason, bool apply_expiry_action);
    bool finish_session();
    bool set_account_disabled(const std::string& sid, bool disabled) const;
    bool set_account_expiry(const std::string& sid, std::int64_t unix_seconds) const;
    bool end_user_sessions(const std::vector<std::string>& sids, bool logoff) const;
    void force_end_session(const std::string& reason);
    void reconcile_managed_accounts();
    [[nodiscard]] bool is_managed(const std::string& sid) const;
    void on_failure(const InspectionFailure& failure);
    void audit_limited(const std::string& key, const std::string& actor, const std::string& event,
                       core::Json detail);
    void timer_loop(const std::stop_token& stop);
    void on_denial(const Denial& denial);
    void on_mode(SupervisionMode mode, const std::string& reason);
    core::Json supervisor_json() const;
    core::Json status_json() const;
    core::Profile& find_profile(const std::string& id);
    bool session_uses(const std::string& profile_id) const;

    std::mutex mutex_;
    std::condition_variable_any timer_wake_;
    Store store_;
    std::unique_ptr<AuditLog> audit_;
    IdentityResolver resolver_;
    std::unique_ptr<Supervisor> supervisor_;
    std::unordered_map<std::string, Command> commands_;
    std::mutex token_mutex_;
    std::unordered_map<std::string, Token> tokens_;
    std::mutex limited_mutex_;
    std::unordered_map<std::string, std::pair<std::chrono::steady_clock::time_point, std::uint64_t>>
        limited_notice_;
    std::mutex failure_mutex_;
    std::unordered_map<std::string, std::pair<std::chrono::steady_clock::time_point, std::uint64_t>>
        failure_notice_;
    std::function<core::Json()> ipc_stats_;
    unsigned end_attempts_ = 0;
    bool live_ = true;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> last_notice_;
    std::mutex notice_mutex_;
    std::filesystem::path install_dir_;
    system::Capabilities capabilities_;
    LoadResult load_result_ = LoadResult::Fresh;
    std::atomic<std::uint64_t> blocked_ = 0;
    std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
    std::jthread timer_;
};

}  // namespace houseguest::service
