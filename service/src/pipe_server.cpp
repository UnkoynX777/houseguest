#include "pipe_server.hpp"

#include <windows.h>

#include <sddl.h>

#include <memory>

#include "houseguest/system/pipe.hpp"
#include "houseguest/system/text.hpp"

namespace houseguest::service {
namespace {

constexpr int kMaxConnectionsPerUser = 4;
constexpr DWORD kMaxInstances = 32;

HANDLE create_instance(bool first) {
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, FALSE};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;AU)",
                                                              SDDL_REVISION_1,
                                                              &attributes.lpSecurityDescriptor, nullptr)) {
        return INVALID_HANDLE_VALUE;
    }
    DWORD open_mode = PIPE_ACCESS_DUPLEX;
    if (first) open_mode |= FILE_FLAG_FIRST_PIPE_INSTANCE;
    HANDLE pipe =
        CreateNamedPipeW(system::kPipeName, open_mode,
                         PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                         kMaxInstances, 65536, 65536, 0, &attributes);
    LocalFree(attributes.lpSecurityDescriptor);
    return pipe;
}

std::optional<Caller> resolve_caller(HANDLE pipe) {
    if (!ImpersonateNamedPipeClient(pipe)) return std::nullopt;
    HANDLE token = nullptr;
    const bool opened = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token);
    RevertToSelf();
    if (!opened) return std::nullopt;
    std::unique_ptr<void, decltype(&CloseHandle)> guard(token, &CloseHandle);

    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<std::uint8_t> buffer(size);
    if (size == 0 || !GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) return std::nullopt;
    LPWSTR text = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &text))
        return std::nullopt;
    Caller caller;
    caller.sid = system::to_utf8(text);
    LocalFree(text);

    TOKEN_ELEVATION elevation{};
    DWORD returned = 0;
    if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &returned)) {
        caller.elevated = elevation.TokenIsElevated != 0;
    }
    return caller;
}

std::optional<std::string> read_message(HANDLE pipe) {
    std::string message;
    std::string chunk(65536, '\0');
    for (;;) {
        DWORD read = 0;
        const BOOL ok = ReadFile(pipe, chunk.data(), static_cast<DWORD>(chunk.size()), &read, nullptr);
        message.append(chunk.data(), read);
        if (message.size() > system::kMaxMessageBytes) return std::nullopt;
        if (ok) return message;
        if (GetLastError() != ERROR_MORE_DATA) return std::nullopt;
    }
}

bool write_message(HANDLE pipe, const std::string& body) {
    DWORD written = 0;
    return WriteFile(pipe, body.data(), static_cast<DWORD>(body.size()), &written, nullptr) &&
           written == body.size();
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
}

bool PipeServer::start(std::string& error) {
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
    HANDLE wake =
        CreateFileW(system::kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (wake != INVALID_HANDLE_VALUE) CloseHandle(wake);
    if (acceptor_.joinable()) acceptor_.join();

    std::unique_lock lock(mutex_);
    for (void* connection : connections_) DisconnectNamedPipe(connection);
    idle_.wait(lock, [this] { return connections_.empty(); });
}

void PipeServer::accept_loop() {
    HANDLE pipe = static_cast<HANDLE>(first_instance_);
    first_instance_ = nullptr;
    while (!stopping_) {
        if (pipe == INVALID_HANDLE_VALUE) {
            pipe = create_instance(false);
            if (pipe == INVALID_HANDLE_VALUE) {
                Sleep(200);
                continue;
            }
        }
        const bool connected = ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
        if (!connected || stopping_) {
            CloseHandle(pipe);
            pipe = INVALID_HANDLE_VALUE;
            continue;
        }
        {
            std::scoped_lock lock(mutex_);
            connections_.insert(pipe);
        }
        std::thread([this, pipe] { serve(pipe); }).detach();
        pipe = INVALID_HANDLE_VALUE;
    }
    if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
}

void PipeServer::serve(void* raw) {
    HANDLE pipe = static_cast<HANDLE>(raw);
    std::optional<Caller> caller;
    bool counted = false;

    while (auto message = read_message(pipe)) {
        if (!caller) {
            caller = resolve_caller(pipe);
            if (!caller) break;
            std::scoped_lock lock(mutex_);
            if (per_user_[caller->sid] >= kMaxConnectionsPerUser) {
                write_message(pipe, error_reply("busy", "Too many open connections.").dump());
                break;
            }
            ++per_user_[caller->sid];
            counted = true;
        }
        const auto request = core::Json::parse(*message, nullptr, false);
        const auto reply = request.is_discarded() ? error_reply("bad_request", "Malformed JSON.")
                                                  : controller_.handle(*caller, request);
        if (!write_message(pipe, reply.dump())) break;
    }

    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
    {
        std::scoped_lock lock(mutex_);
        if (counted && caller) --per_user_[caller->sid];
        connections_.erase(raw);
    }
    idle_.notify_all();
}

}  // namespace houseguest::service
