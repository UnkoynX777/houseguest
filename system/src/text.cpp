#include "houseguest/system/text.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>

namespace houseguest::system {

std::string to_utf8(std::wstring_view wide) {
    if (wide.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0,
                                         nullptr, nullptr);
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), size, nullptr,
                        nullptr);
    return out;
}

std::wstring to_wide(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), size);
    return out;
}

std::string path_to_utf8(const std::filesystem::path& path) {
    return to_utf8(path.native());
}

std::filesystem::path path_from_utf8(std::string_view utf8) {
    return to_wide(utf8);
}

std::string expand_known_folders(std::string_view pattern) {
    struct Variable {
        std::string_view token;
        const wchar_t* environment;
    };
    constexpr std::array<Variable, 5> variables{{{"%systemroot%", L"SystemRoot"},
                                                 {"%systemdrive%", L"SystemDrive"},
                                                 {"%programfiles%", L"ProgramFiles"},
                                                 {"%programfiles(x86)%", L"ProgramFiles(x86)"},
                                                 {"%programdata%", L"ProgramData"}}};
    std::string out(pattern);
    for (const auto& [token, environment] : variables) {
        for (;;) {
            std::string lowered(out);
            std::ranges::transform(lowered, lowered.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const auto at = lowered.find(token);
            if (at == std::string::npos) break;
            std::wstring value(MAX_PATH, L'\0');
            const DWORD length =
                GetEnvironmentVariableW(environment, value.data(), static_cast<DWORD>(value.size()));
            value.resize(length);
            out.replace(at, token.size(), to_utf8(value));
        }
    }
    return out;
}

}  // namespace houseguest::system
