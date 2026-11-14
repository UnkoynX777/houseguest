#include <windows.h>

#include "houseguest/system/pipe.hpp"

namespace houseguest::system {

std::wstring instance_suffix() {
#ifdef HOUSEGUEST_INSTANCES
    wchar_t value[17] = {};
    const DWORD length = GetEnvironmentVariableW(L"HOUSEGUEST_INSTANCE", value, 17);
    if (length == 0 || length > 16) return {};
    for (DWORD i = 0; i < length; ++i) {
        if (!iswalnum(value[i])) return {};
    }
    return std::wstring(L"-") + value;
#else
    return {};
#endif
}

std::wstring pipe_name() {
    return L"\\\\.\\pipe\\houseguest" + instance_suffix();
}

struct PipeClient::Impl {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    std::wstring name = pipe_name();

    bool connect() {
        if (pipe != INVALID_HANDLE_VALUE) return true;
        for (int attempt = 0; attempt < 3; ++attempt) {
            pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                               SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
            if (pipe != INVALID_HANDLE_VALUE) break;
            if (GetLastError() != ERROR_PIPE_BUSY || !WaitNamedPipeW(name.c_str(), 2000)) return false;
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
