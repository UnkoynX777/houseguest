#include "pipe_server.hpp"

#include <windows.h>

#include <sddl.h>

#include <algorithm>
#include <memory>
#include <system_error>

#include "houseguest/system/pipe.hpp"
#include "houseguest/system/text.hpp"

#include "ipc_limits.hpp"

namespace houseguest::service {
namespace {

using std::chrono::milliseconds;
using std::chrono::seconds;

constexpr int kMaxConnectionsPerUser = 4;
constexpr int kMaxActive = 24;
constexpr int kMaxPending = 16;
constexpr int kMaxPendingPerUser = 3;
constexpr DWORD kMaxInstances = 32;
constexpr DWORD kBufferBytes = 65536;
constexpr auto kFirstMessageTimeout = seconds(5);
constexpr auto kMessageTimeout = seconds(10);
constexpr auto kIdleTimeout = seconds(120);
constexpr auto kWriteTimeout = seconds(5);
constexpr double kRequestsPerSecond = 20;
constexpr double kRequestBurst = 40;
constexpr int kMaxRateViolations = 100;

class Owned {
public:
    explicit Owned(HANDLE handle = nullptr) : handle_(handle) {}
    ~Owned() { reset(); }
    Owned(const Owned&) = delete;
    Owned& operator=(const Owned&) = delete;
    [[nodiscard]] HANDLE get() const { return handle_; }
    void reset(HANDLE handle = nullptr) {
        if (handle_ && handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
        handle_ = handle;
    }

private:
    HANDLE handle_;
};

HANDLE create_instance(bool first) {
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, FALSE};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;AU)",
                                                              SDDL_REVISION_1,
                                                              &attributes.lpSecurityDescriptor, nullptr)) {
        return INVALID_HANDLE_VALUE;
    }
    DWORD open_mode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED;
    if (first) open_mode |= FILE_FLAG_FIRST_PIPE_INSTANCE;
    HANDLE pipe =
        CreateNamedPipeW(system::pipe_name().c_str(), open_mode,
                         PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                         kMaxInstances, kBufferBytes, kBufferBytes, 0, &attributes);
    LocalFree(attributes.lpSecurityDescriptor);
    return pipe;
}

enum class Io { Done, More, Failed, TimedOut, Stopped };

Io finish(HANDLE pipe, OVERLAPPED& overlapped, HANDLE stop, std::chrono::steady_clock::time_point deadline,
          DWORD& bytes) {
    const HANDLE waitable[] = {overlapped.hEvent, stop};
    const auto remaining =
        std::chrono::duration_cast<milliseconds>(deadline - std::chrono::steady_clock::now());
    const DWORD timeout = remaining.count() <= 0 ? 0 : static_cast<DWORD>(remaining.count());
    const DWORD waited = WaitForMultipleObjects(2, waitable, FALSE, timeout);
    if (waited != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &overlapped);
        DWORD ignored = 0;
        GetOverlappedResult(pipe, &overlapped, &ignored, TRUE);
        return waited == WAIT_OBJECT_0 + 1 ? Io::Stopped : Io::TimedOut;
    }
    if (GetOverlappedResult(pipe, &overlapped, &bytes, FALSE)) return Io::Done;
    return GetLastError() == ERROR_MORE_DATA ? Io::More : Io::Failed;
}

Io read_chunk(HANDLE pipe, HANDLE event, HANDLE stop, std::chrono::steady_clock::time_point deadline,
              char* buffer, DWORD capacity, DWORD& bytes) {
    OVERLAPPED overlapped{};
    overlapped.hEvent = event;
    ResetEvent(event);
    bytes = 0;
    if (ReadFile(pipe, buffer, capacity, nullptr, &overlapped))
        return finish(pipe, overlapped, stop, deadline, bytes);
    const DWORD error = GetLastError();
    if (error == ERROR_IO_PENDING) return finish(pipe, overlapped, stop, deadline, bytes);
    if (error == ERROR_MORE_DATA) return finish(pipe, overlapped, stop, deadline, bytes);
    return Io::Failed;
}

enum class Read { Message, TimedOut, Oversized, Closed };

struct Received {
    Read status = Read::Closed;
    std::string body;
};

Received read_message(HANDLE pipe, HANDLE event, HANDLE stop, std::chrono::seconds first_wait) {
    Received received;
    std::string chunk(kBufferBytes, '\0');
    auto deadline = std::chrono::steady_clock::now() + first_wait;
    bool started = false;
    for (;;) {
        DWORD bytes = 0;
        const Io io =
            read_chunk(pipe, event, stop, deadline, chunk.data(), static_cast<DWORD>(chunk.size()), bytes);
        if (io == Io::TimedOut) {
            received.status = Read::TimedOut;
            return received;
        }
        if (io == Io::Failed || io == Io::Stopped) return received;
        if (!started) {
            started = true;
            deadline = std::chrono::steady_clock::now() + kMessageTimeout;
        }
        if (received.body.size() + bytes > system::kMaxMessageBytes) {
            received.status = Read::Oversized;
            return received;
        }
        received.body.append(chunk.data(), bytes);
        if (io == Io::Done) {
            received.status = Read::Message;
            return received;
        }
    }
}

bool write_message(HANDLE pipe, HANDLE event, HANDLE stop, const std::string& body) {
    OVERLAPPED overlapped{};
    overlapped.hEvent = event;
    ResetEvent(event);
    DWORD written = 0;
    const auto deadline = std::chrono::steady_clock::now() + kWriteTimeout;
    if (!WriteFile(pipe, body.data(), static_cast<DWORD>(body.size()), nullptr, &overlapped)) {
        if (GetLastError() != ERROR_IO_PENDING) return false;
    }
    return finish(pipe, overlapped, stop, deadline, written) == Io::Done && written == body.size();
}

std::optional<Caller> resolve_caller(HANDLE pipe) {
    if (!ImpersonateNamedPipeClient(pipe)) return std::nullopt;
    HANDLE raw = nullptr;
    const bool opened = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &raw);
    RevertToSelf();
    if (!opened) return std::nullopt;
    Owned token(raw);

    DWORD size = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
    std::vector<std::uint8_t> buffer(size);
    if (size == 0 || !GetTokenInformation(token.get(), TokenUser, buffer.data(), size, &size))
        return std::nullopt;
    LPWSTR text = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &text))
        return std::nullopt;
    Caller caller;
    caller.sid = system::to_utf8(text);
    LocalFree(text);

    TOKEN_ELEVATION elevation{};
    DWORD returned = 0;
    if (GetTokenInformation(token.get(), TokenElevation, &elevation, sizeof(elevation), &returned)) {
        caller.elevated = elevation.TokenIsElevated != 0;
    }
    return caller;
}

std::string peer_identity(HANDLE pipe) {
    ULONG pid = 0;
    if (!GetNamedPipeClientProcessId(pipe, &pid)) return "unknown";
    Owned process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process.get()) return "unknown";
    HANDLE raw = nullptr;
    if (!OpenProcessToken(process.get(), TOKEN_QUERY, &raw)) return "unknown";
    Owned token(raw);
    DWORD size = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
    std::vector<std::uint8_t> buffer(size);
    if (size == 0 || !GetTokenInformation(token.get(), TokenUser, buffer.data(), size, &size))
        return "unknown";
    LPWSTR text = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &text))
        return "unknown";
    std::string sid = system::to_utf8(text);
    LocalFree(text);
    return sid;
}

core::Json error_reply(const char* code, const char* message) {
    return {{"id", nullptr},
            {"ok", false},
            {"error", {{"code", code}, {"message", message}, {"details", core::Json::object()}}}};
}

}  // namespace

PipeServer::PipeServer(Controller& controller) : controller_(controller) {}

PipeServer::~PipeServer() {
    stop();
    if (stop_event_) CloseHandle(stop_event_);
}

bool PipeServer::start(std::string& error) {
    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop_event_) {
        error = "cannot create the stop event";
        return false;
    }
    first_instance_ = create_instance(true);
    if (first_instance_ == INVALID_HANDLE_VALUE) {
        error =
            "cannot create the pipe (is another instance running?) code " + std::to_string(GetLastError());
        return false;
    }
    acceptor_ = std::thread([this] { accept_loop(); });
    return true;
}

void PipeServer::stop() {
    if (stopping_.exchange(true)) return;
    if (stop_event_) SetEvent(stop_event_);
    if (acceptor_.joinable()) acceptor_.join();
    std::unique_lock lock(mutex_);
    idle_.wait(lock, [this] { return active_ == 0; });
}

core::Json PipeServer::stats() const {
    std::scoped_lock lock(mutex_);
    return {{"active", active_},
            {"pending", pending_},
            {"accepted", accepted_.load()},
            {"rejected_busy", rejected_busy_.load()},
            {"timed_out", timed_out_.load()},
            {"malformed", malformed_.load()},
            {"oversized", oversized_.load()},
            {"rate_limited", rate_limited_.load()}};
}

bool PipeServer::admit(const std::string& peer) {
    std::scoped_lock lock(mutex_);
    auto& usage = per_user_[peer];
    const bool allowed = active_ < kMaxActive && pending_ < kMaxPending &&
                         usage.active < kMaxConnectionsPerUser && usage.pending < kMaxPendingPerUser;
    if (!allowed) {
        if (usage.active == 0 && usage.pending == 0) per_user_.erase(peer);
        return false;
    }
    ++active_;
    ++pending_;
    ++usage.active;
    ++usage.pending;
    return true;
}

void PipeServer::leave_pending(const std::string& peer) {
    std::scoped_lock lock(mutex_);
    if (pending_ > 0) --pending_;
    const auto it = per_user_.find(peer);
    if (it != per_user_.end() && it->second.pending > 0) --it->second.pending;
}

void PipeServer::release(const std::string& peer, bool was_pending) {
    {
        std::scoped_lock lock(mutex_);
        if (was_pending && pending_ > 0) --pending_;
        const auto it = per_user_.find(peer);
        if (it != per_user_.end()) {
            if (was_pending && it->second.pending > 0) --it->second.pending;
            if (it->second.active > 0) --it->second.active;
            if (it->second.active == 0 && it->second.pending == 0) per_user_.erase(it);
        }
        --active_;
    }
    idle_.notify_all();
}

void PipeServer::accept_loop() {
    HANDLE pipe = static_cast<HANDLE>(first_instance_);
    first_instance_ = nullptr;
    Owned connect_event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    HANDLE stop = static_cast<HANDLE>(stop_event_);

    while (!stopping_) {
        if (pipe == INVALID_HANDLE_VALUE) {
            pipe = create_instance(false);
            if (pipe == INVALID_HANDLE_VALUE) {
                if (WaitForSingleObject(stop, 200) == WAIT_OBJECT_0) break;
                continue;
            }
        }
        OVERLAPPED overlapped{};
        overlapped.hEvent = connect_event.get();
        ResetEvent(connect_event.get());
        bool connected = false;
        if (ConnectNamedPipe(pipe, &overlapped)) {
            connected = true;
        } else {
            const DWORD error = GetLastError();
            if (error == ERROR_PIPE_CONNECTED) {
                connected = true;
            } else if (error == ERROR_IO_PENDING) {
                const HANDLE waitable[] = {connect_event.get(), stop};
                if (WaitForMultipleObjects(2, waitable, FALSE, INFINITE) == WAIT_OBJECT_0) {
                    DWORD ignored = 0;
                    connected = GetOverlappedResult(pipe, &overlapped, &ignored, FALSE) != FALSE;
                } else {
                    CancelIoEx(pipe, &overlapped);
                    DWORD ignored = 0;
                    GetOverlappedResult(pipe, &overlapped, &ignored, TRUE);
                }
            }
        }
        if (!connected || stopping_) {
            CloseHandle(pipe);
            pipe = INVALID_HANDLE_VALUE;
            continue;
        }
        ++accepted_;
        const auto peer = peer_identity(pipe);
        if (!admit(peer)) {
            ++rejected_busy_;
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            pipe = INVALID_HANDLE_VALUE;
            continue;
        }
        try {
            std::thread([this, pipe, peer] { serve(pipe, peer); }).detach();
            pipe = INVALID_HANDLE_VALUE;
        } catch (const std::system_error&) {
            release(peer, true);
            ++rejected_busy_;
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            pipe = INVALID_HANDLE_VALUE;
        }
    }
    if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
}

void PipeServer::serve(void* raw, const std::string& peer) {
    Owned pipe(static_cast<HANDLE>(raw));
    Owned event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    HANDLE stop = static_cast<HANDLE>(stop_event_);
    std::optional<Caller> caller;
    bool pending = true;
    TokenBucket bucket(kRequestsPerSecond, kRequestBurst);
    int violations = 0;

    while (event.get() && !stopping_) {
        auto received =
            read_message(pipe.get(), event.get(), stop, pending ? kFirstMessageTimeout : kIdleTimeout);
        if (received.status == Read::TimedOut) {
            ++timed_out_;
            break;
        }
        if (received.status == Read::Oversized) {
            ++oversized_;
            write_message(pipe.get(), event.get(), stop,
                          error_reply("too_large", "Message exceeds the size limit.").dump());
            break;
        }
        if (received.status != Read::Message) break;

        if (pending) {
            pending = false;
            leave_pending(peer);
        }
        if (!caller) {
            caller = resolve_caller(pipe.get());
            if (!caller) break;
        }

        core::Json reply;
        core::Json parsed;
        if (!bucket.take()) {
            ++rate_limited_;
            reply = error_reply("rate_limited", "Too many requests.");
            if (++violations >= kMaxRateViolations) break;
        } else if (received.body.empty() || !nesting_within(received.body, kMaxJsonDepth)) {
            ++malformed_;
            reply = error_reply("bad_request", "Malformed message.");
        } else {
            const auto request = core::Json::parse(received.body, nullptr, false);
            if (request.is_discarded()) {
                ++malformed_;
                reply = error_reply("bad_request", "Malformed JSON.");
            } else {
                parsed = request;
                reply = controller_.handle(*caller, request);
            }
        }
        const auto encoding_started = std::chrono::steady_clock::now();
        const auto encoded = reply.dump();
        controller_.note_reply(parsed, encoded.size(), std::chrono::steady_clock::now() - encoding_started);
        if (!write_message(pipe.get(), event.get(), stop, encoded)) break;
    }

    if (pipe.get() != INVALID_HANDLE_VALUE) DisconnectNamedPipe(pipe.get());
    pipe.reset();
    event.reset();
    release(peer, pending);
}

}  // namespace houseguest::service
