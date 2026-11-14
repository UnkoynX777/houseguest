#pragma once

#include <windows.h>

#include <sddl.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "houseguest/system/text.hpp"

namespace houseguest::system {

struct HandleCloser {
    void operator()(HANDLE h) const {
        if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
};
using Handle = std::unique_ptr<void, HandleCloser>;

struct LocalFreeDeleter {
    void operator()(void* p) const { LocalFree(p); }
};

inline std::optional<std::string> sid_string(PSID sid) {
    LPWSTR text = nullptr;
    if (!ConvertSidToStringSidW(sid, &text)) return std::nullopt;
    std::unique_ptr<void, LocalFreeDeleter> guard(text);
    return to_utf8(text);
}

inline std::optional<std::string> token_user_sid(HANDLE token) {
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    if (size == 0) return std::nullopt;
    std::vector<std::uint8_t> buffer(size);
    if (!GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) return std::nullopt;
    return sid_string(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid);
}

}  // namespace houseguest::system
