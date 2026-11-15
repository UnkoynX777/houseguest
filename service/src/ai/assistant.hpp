#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "../ipc_limits.hpp"
#include "chat.hpp"

namespace houseguest::service::ai {

// What the assistant may ask of the service. Reads are fixed, named questions; changes exist only as proposals the
// owner confirms, and confirmation arrives through a different command that a model response cannot trigger.
class AssistantHost {
public:
    virtual ~AssistantHost() = default;
    virtual core::Json read(const std::string& what, const core::Json& args) = 0;
    // Checks and simulates a change. Throws a service Failure for anything invalid or unsafe.
    virtual core::Json prepare(const core::Json& operation) = 0;
    virtual core::Json apply(const core::Json& operation, core::Json& rollback) = 0;
    virtual void undo(const core::Json& rollback) = 0;
    virtual void audit(const std::string& event, core::Json detail) = 0;
};

struct AssistantSettings {
    bool enabled = false;
    std::string model = "gemini-3.1-flash-lite";
    bool share_context = false;
};

struct Proposal {
    std::string id;
    std::string chat_id;
    std::string kind;
    std::string summary;
    core::Json operation;
    core::Json preview;
    core::Json rollback;
    std::string status = "pending";  // pending, applied, discarded, rolled_back, expired
    std::int64_t created = 0;
};

class Assistant final : public ToolHost {
public:
    Assistant(AssistantHost& host, std::filesystem::path directory, StreamTransport transport);
    ~Assistant();

    [[nodiscard]] core::Json status() const;
    core::Json set_key(const std::string& api_key);
    void clear_key();
    core::Json set_settings(const core::Json& changes);
    [[nodiscard]] core::Json models();

    // Starts a conversation turn in the background and returns the job id.
    std::string send(const std::string& chat_id, const std::string& text, const core::Json& context);
    [[nodiscard]] core::Json job(const std::string& id, std::size_t since) const;
    void cancel(const std::string& id);

    [[nodiscard]] core::Json chats() const;
    [[nodiscard]] core::Json chat(const std::string& id) const;
    void delete_chat(const std::string& id);

    [[nodiscard]] core::Json proposals() const;
    core::Json apply_proposal(const std::string& id);
    void discard_proposal(const std::string& id);
    core::Json rollback_proposal(const std::string& id);

    [[nodiscard]] core::Json declarations() const override;
    core::Json call(const std::string& name, const core::Json& args) override;

    void shutdown();

    static constexpr int kPerMinute = 10;
    static constexpr int kPerDay = 400;
    static constexpr std::int64_t kProposalLifetime = 1800;

private:
    struct Job {
        std::string id;
        std::string chat_id;
        std::vector<core::Json> events;
        bool done = false;
        std::atomic<bool> cancel = false;
        std::thread thread;
    };
    void run_job(const std::shared_ptr<Job>& job, std::string text, core::Json context);
    void push(const std::shared_ptr<Job>& job, core::Json event);
    void load();
    void save();
    void save_chats();
    [[nodiscard]] bool take_quota(std::string& error);
    core::Json add_proposal(const std::string& kind, const core::Json& operation);
    void expire_locked(std::int64_t now);

    AssistantHost& host_;
    std::filesystem::path settings_file_, chats_file_;
    StreamTransport transport_;
    mutable std::mutex mutex_;
    AssistantSettings settings_;
    std::string key_;
    std::map<std::string, std::shared_ptr<Job>> jobs_;
    std::deque<std::string> job_order_;
    struct Chat {
        std::string id, title;
        std::int64_t updated = 0;
        core::Json history = core::Json::array();
        core::Json messages = core::Json::array();
    };
    std::map<std::string, Chat> chats_;
    std::map<std::string, Proposal> proposals_;
    std::string active_chat_;
    TokenBucket bucket_{kPerMinute / 60.0, kPerMinute};
    std::int64_t day_start_ = 0;
    int day_count_ = 0;
};

[[nodiscard]] std::string clean_text(const std::string& text, std::size_t limit);

}  // namespace houseguest::service::ai
