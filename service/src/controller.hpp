#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "houseguest/system/capabilities.hpp"

#include "api/service.hpp"
#include "assessor.hpp"
#include "audit.hpp"
#include "catalog.hpp"
#include "change_watcher.hpp"
#include "continuity.hpp"
#include "discovery.hpp"
#include "file_details.hpp"
#include "lineage.hpp"
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
    void note_reply(const core::Json& request, std::size_t bytes, std::chrono::steady_clock::duration encoding);

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
    void register_app_commands();
    void register_catalog_commands();
    void register_api_commands();
    class ApiBridge;
    void start_api();
    core::Json run_api(const std::string& command, const core::Json& args, const std::string& device_id);
    bool api_check_pin(const std::string& pin);
    system::CatalogRequest catalog_request();
    system::CatalogRequest catalog_request_locked() const;
    void add(const char* name, Access access, std::function<core::Json(const Caller&, const core::Json&)> run,
             bool unlocked = false);
    void require_owner(const Caller& caller, const core::Json& request);
    std::string verify_pin_and_issue_token(const Caller& caller, const std::string& pin);
    void verify_pin(const std::string& pin);
    bool pin_matches(const std::string& pin) const;
    void set_pin(const std::string& pin);
    std::string issue_token(const std::string& sid);

    Compiled compile(const core::Profile& profile, const std::vector<core::Rule>& exceptions,
                     const std::vector<std::string>& guests, bool audit_only);
    void store_profile(const std::string& actor, core::Profile profile, const char* event);
    core::Decision decide_file(const EnforcementPolicy& policy, const std::filesystem::path& file,
                               const std::string& package_family, std::vector<std::string> lineage);
    core::Json explain(const core::Profile& profile, const std::filesystem::path& file,
                       const std::string& package_family);
    core::Json blocks(std::size_t limit);
    core::Json approve_component(const std::string& actor, const std::string& profile_id,
                                 const std::string& app_id, const std::string& path, const core::Json& args);
    void remember_review(const Denial& denial);
    struct Review {
        std::string id, app_id, kind, path, sha256, publisher, package_family, parent_path, profile_id, note;
        std::uint64_t count = 0;
        std::int64_t first = 0, last = 0;
    };
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
    void on_allowed(const Denial& allowed);
    void on_mode(SupervisionMode mode, const std::string& reason);
    core::Json supervisor_json() const;
    core::Json status_json() const;
    core::Profile& find_profile(const std::string& id);
    bool session_uses(const std::string& profile_id) const;

    std::mutex mutex_;
    std::condition_variable_any timer_wake_;
    Store store_;
    AppCatalog catalog_;
    std::unique_ptr<ChangeWatcher> watcher_;
    std::filesystem::path install_dir_;
    std::unique_ptr<AuditLog> audit_;
    IdentityResolver resolver_;
    FileDetails details_;
    ContinuityGuard continuity_;
    Lineage lineage_;
    Assessor assessor_;
    Discovery discovery_;
    std::unique_ptr<Supervisor> supervisor_;
    std::mutex review_mutex_;
    std::map<std::string, Review> reviews_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> notified_;
    std::unordered_map<std::string, Command> commands_;
    std::mutex token_mutex_;
    std::unordered_map<std::string, Token> tokens_;
    std::mutex limited_mutex_;
    std::mutex allowed_mutex_;
    std::unordered_set<std::string> allowed_seen_;
    std::unordered_map<std::string, std::pair<std::chrono::steady_clock::time_point, std::uint64_t>>
        limited_notice_;
    std::mutex failure_mutex_;
    std::unordered_map<std::string, std::pair<std::chrono::steady_clock::time_point, std::uint64_t>>
        failure_notice_;
    std::function<core::Json()> ipc_stats_;
    struct CommandStat {
        std::uint64_t count = 0;
        std::uint64_t run_us = 0;
        std::uint64_t run_max_us = 0;
        std::uint64_t encode_us = 0;
        std::uint64_t bytes = 0;
        std::uint64_t bytes_max = 0;
    };
    std::mutex stats_mutex_;
    std::map<std::string, CommandStat> command_stats_;

    // Presentation cache for the library: the compiled profile and the decision for each listed program, valid
    // for one revision of the stored policy and for the file's size and write time. It never feeds enforcement;
    // the supervisor evaluates against the policy it was handed and re-evaluates when that policy is replaced.
    struct CachedDecision {
        std::int64_t size = 0;
        std::int64_t modified = 0;
        core::Decision decision;
    };
    struct ProfileCache {
        std::uint64_t revision = 0;
        std::shared_ptr<const Compiled> compiled;
        std::unordered_map<std::string, CachedDecision> decisions;
    };
    std::mutex library_mutex_;
    std::map<std::string, ProfileCache> library_cache_;
    std::atomic<std::uint64_t> library_hits_{0};
    std::atomic<std::uint64_t> library_misses_{0};
    [[nodiscard]] core::Json command_stats_json();
    [[nodiscard]] core::Json caches_json();
    unsigned end_attempts_ = 0;
    bool live_ = true;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> last_notice_;
    std::mutex notice_mutex_;
    system::Capabilities capabilities_;
    LoadResult load_result_ = LoadResult::Fresh;
    std::atomic<std::uint64_t> blocked_ = 0;
    std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
    std::jthread timer_;
    std::int64_t remote_locked_until_ = 0;
    int remote_failures_ = 0;
    std::shared_ptr<api::Host> api_host_;
    std::unique_ptr<api::ApiService> api_;
};

}  // namespace houseguest::service
