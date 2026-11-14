#include "supervisor.hpp"

#include <windows.h>

#include <evntcons.h>
#include <evntrace.h>

#include <algorithm>
#include <chrono>

#include "houseguest/core/glob.hpp"
#include "houseguest/system/crypto.hpp"
#include "houseguest/system/text.hpp"
#include "houseguest/system/windows_info.hpp"

namespace houseguest::service {
namespace {

constexpr wchar_t kSessionName[] = L"HouseguestProcessTrace";
constexpr unsigned kWorkerCount = 2;
constexpr std::size_t kMaxCacheEntries = 4096;
constexpr std::uint16_t kProcessStartEventId = 1;
constexpr ULONGLONG kProcessKeyword = 0x10;
constexpr GUID kKernelProcessProvider = {
    0x22fb2cd6, 0x0e7b, 0x422b, {0xa0, 0xc7, 0x2f, 0xad, 0x1f, 0xd0, 0xe7, 0x16}};

Supervisor* g_supervisor = nullptr;

std::vector<std::uint8_t> trace_properties_buffer() {
    const std::size_t size = sizeof(EVENT_TRACE_PROPERTIES) + sizeof(kSessionName);
    std::vector<std::uint8_t> buffer(size);
    auto* properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buffer.data());
    properties->Wnode.BufferSize = static_cast<ULONG>(size);
    properties->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    properties->Wnode.ClientContext = 1;
    properties->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    properties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    return buffer;
}

void WINAPI trace_callback(PEVENT_RECORD record) {
    Supervisor::on_event(record);
}

std::string lowered(std::string value) {
    std::ranges::transform(value, value.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

}  // namespace

core::AppIdentity IdentityResolver::resolve(const std::filesystem::path& file, bool with_hash,
                                            bool with_publisher) {
    core::AppIdentity identity;
    identity.path = system::path_to_utf8(file);

    std::error_code ec;
    const auto size = std::filesystem::file_size(file, ec);
    const auto modified = std::filesystem::last_write_time(file, ec);
    const auto key = lowered(identity.path);

    std::scoped_lock lock(mutex_);
    if (cache_.size() > kMaxCacheEntries) cache_.clear();
    auto& entry = cache_[key];
    if (entry.size != size || entry.modified != modified) entry = Entry{size, modified, {}, {}};
    if (with_hash && !entry.sha256) entry.sha256 = system::sha256_file(file).value_or("");
    if (with_publisher && !entry.publisher) entry.publisher = system::trusted_publisher(file);
    identity.sha256 = entry.sha256.value_or("");
    identity.publisher = entry.publisher.value_or("");
    return identity;
}

Supervisor::Supervisor(IdentityResolver& resolver, DenialHandler on_denial)
    : resolver_(resolver), on_denial_(std::move(on_denial)) {}

Supervisor::~Supervisor() {
    stop();
}

bool Supervisor::start(std::string& error) {
    if (running_) return true;
    g_supervisor = this;

    auto buffer = trace_properties_buffer();
    auto* properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buffer.data());
    TRACEHANDLE session = 0;
    ULONG status = StartTraceW(&session, kSessionName, properties);
    if (status == ERROR_ALREADY_EXISTS) {
        auto stale = trace_properties_buffer();
        ControlTraceW(0, kSessionName, reinterpret_cast<EVENT_TRACE_PROPERTIES*>(stale.data()),
                      EVENT_TRACE_CONTROL_STOP);
        status = StartTraceW(&session, kSessionName, properties);
    }
    if (status != ERROR_SUCCESS) {
        error = "StartTrace failed with code " + std::to_string(status);
        return false;
    }
    session_handle_ = session;

    status = EnableTraceEx2(session, &kKernelProcessProvider, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                            TRACE_LEVEL_INFORMATION, kProcessKeyword, 0, 0, nullptr);
    if (status != ERROR_SUCCESS) {
        error = "EnableTraceEx2 failed with code " + std::to_string(status);
        ControlTraceW(session, nullptr, properties, EVENT_TRACE_CONTROL_STOP);
        return false;
    }

    EVENT_TRACE_LOGFILEW log{};
    log.LoggerName = const_cast<LPWSTR>(kSessionName);
    log.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    log.EventRecordCallback = trace_callback;
    const TRACEHANDLE consumer = OpenTraceW(&log);
    if (consumer == INVALID_PROCESSTRACE_HANDLE) {
        error = "OpenTrace failed";
        ControlTraceW(session, nullptr, properties, EVENT_TRACE_CONTROL_STOP);
        return false;
    }
    trace_handle_ = consumer;

    running_ = true;
    for (unsigned i = 0; i < kWorkerCount; ++i) {
        workers_.emplace_back([this](const std::stop_token& stop) { work(stop); });
    }
    trace_thread_ = std::thread([consumer] {
        TRACEHANDLE handle = consumer;
        ProcessTrace(&handle, 1, nullptr, nullptr);
    });
    return true;
}

void Supervisor::stop() {
    if (!running_.exchange(false)) return;
    auto buffer = trace_properties_buffer();
    ControlTraceW(session_handle_, nullptr, reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buffer.data()),
                  EVENT_TRACE_CONTROL_STOP);
    if (trace_thread_.joinable()) trace_thread_.join();
    CloseTrace(trace_handle_);
    for (auto& worker : workers_) worker.request_stop();
    queue_ready_.notify_all();
    workers_.clear();
    g_supervisor = nullptr;
}

void Supervisor::apply(std::shared_ptr<const EnforcementPolicy> policy) {
    std::scoped_lock lock(policy_mutex_);
    policy_ = std::move(policy);
}

void Supervisor::sweep() {
    for (const auto pid : system::running_process_ids()) {
        if (pid > 4) enqueue(pid);
    }
}

void Supervisor::on_event(void* raw) {
    const auto* record = static_cast<const EVENT_RECORD*>(raw);
    if (!g_supervisor || record->EventHeader.EventDescriptor.Id != kProcessStartEventId ||
        !IsEqualGUID(record->EventHeader.ProviderId, kKernelProcessProvider) ||
        record->UserDataLength < sizeof(std::uint32_t)) {
        return;
    }
    g_supervisor->enqueue(*static_cast<const std::uint32_t*>(record->UserData));
}

void Supervisor::enqueue(std::uint32_t pid) {
    {
        std::scoped_lock lock(queue_mutex_);
        queue_.push(pid);
    }
    queue_ready_.notify_one();
}

void Supervisor::work(const std::stop_token& stop) {
    for (;;) {
        std::uint32_t pid = 0;
        {
            std::unique_lock lock(queue_mutex_);
            if (!queue_ready_.wait(lock, stop, [this] { return !queue_.empty(); })) return;
            pid = queue_.front();
            queue_.pop();
        }
        try {
            inspect(pid);
        } catch (const std::exception&) {
        }
    }
}

void Supervisor::inspect(std::uint32_t pid) {
    std::shared_ptr<const EnforcementPolicy> policy;
    {
        std::scoped_lock lock(policy_mutex_);
        policy = policy_;
    }
    if (!policy || pid == GetCurrentProcessId()) return;

    const auto sid = system::process_user_sid(pid);
    if (!sid || !policy->guest_sids.contains(*sid)) return;
    const auto image = system::process_image_path(pid);
    if (!image) return;

    const auto now = std::chrono::system_clock::now();
    auto identity = resolver_.resolve(*image, false, false);
    auto decision = core::evaluate(policy->profile, policy->baseline, identity, now);
    const bool settled_by_path = decision.source == core::DecisionSource::Baseline ||
                                 (decision.source == core::DecisionSource::Rule &&
                                  (decision.priority > policy->highest_expensive_priority ||
                                   (decision.priority == policy->highest_expensive_priority &&
                                    decision.action == core::Action::Deny)));
    if (!settled_by_path && (policy->needs_hash || policy->needs_publisher)) {
        identity = resolver_.resolve(*image, policy->needs_hash, policy->needs_publisher);
        decision = core::evaluate(policy->profile, policy->baseline, identity, now);
    }
    if (decision.action == core::Action::Allow) return;

    DWORD session_id = 0;
    ProcessIdToSessionId(pid, &session_id);
    const bool enforce = !policy->audit_only;
    if (enforce) system::terminate_process(pid);
    on_denial_({pid, session_id, *sid, identity, decision, enforce});
}

}  // namespace houseguest::service
