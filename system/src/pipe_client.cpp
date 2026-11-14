#include <windows.h>

#include "houseguest/system/pipe.hpp"

namespace houseguest::system {

struct PipeClient::Impl {
    HANDLE pipe = INVALID_HANDLE_VALUE;

    bool connect() {
        if (pipe != INVALID_HANDLE_VALUE) return true;
        for (int attempt = 0; attempt < 3; ++attempt) {
            pipe = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                               SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
            if (pipe != INVALID_HANDLE_VALUE) break;
            if (GetLastError() != ERROR_PIPE_BUSY || !WaitNamedPipeW(kPipeName, 2000)) return false;
        }
        if (pipe == INVALID_HANDLE_VALUE) return false;
        DWORD mode = PIPE_READMODE_MESSAGE;
        SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);
        return true;
    }

    void disconnect() {
        if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
        pipe = INVALID_HANDLE_VALUE;
    }
};

PipeClient::PipeClient() : impl_(std::make_unique<Impl>()) {}
PipeClient::~PipeClient() {
    impl_->disconnect();
}

std::optional<core::Json> PipeClient::call(const core::Json& request) {
    if (!impl_->connect()) return std::nullopt;
    const auto body = request.dump();
    DWORD written = 0;
    if (!WriteFile(impl_->pipe, body.data(), static_cast<DWORD>(body.size()), &written, nullptr)) {
        impl_->disconnect();
        return std::nullopt;
    }

    std::string reply;
    std::string chunk(65536, '\0');
    for (;;) {
        DWORD read = 0;
        const BOOL ok = ReadFile(impl_->pipe, chunk.data(), static_cast<DWORD>(chunk.size()), &read, nullptr);
        reply.append(chunk.data(), read);
        if (ok) break;
        if (GetLastError() != ERROR_MORE_DATA || reply.size() > kMaxMessageBytes * 4) {
            impl_->disconnect();
            return std::nullopt;
        }
    }
    auto parsed = core::Json::parse(reply, nullptr, false);
    if (parsed.is_discarded()) return std::nullopt;
    return parsed;
}

}  // namespace houseguest::system
