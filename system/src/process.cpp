#include "houseguest/system/process.hpp"

#include <windows.h>

#include <tlhelp32.h>

#include "win_util.hpp"

namespace houseguest::system {
namespace {

constexpr DWORD kAccess = PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE;

std::uint64_t ticks_of(const FILETIME& time) {
    return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

}  // namespace

std::optional<ProcessRef> ProcessRef::open(std::uint32_t pid) {
    HANDLE raw = OpenProcess(kAccess, FALSE, pid);
    if (!raw) raw = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!raw) return std::nullopt;
    return ProcessRef(pid, std::shared_ptr<void>(raw, HandleCloser{}));
}

std::uint64_t ProcessRef::now_ticks() {
    FILETIME now{};
    GetSystemTimePreciseAsFileTime(&now);
    return ticks_of(now);
}

std::optional<std::string> ProcessRef::user_sid() const {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(handle_.get(), TOKEN_QUERY, &raw)) return std::nullopt;
    Handle token(raw);
    return token_user_sid(token.get());
}

std::optional<std::filesystem::path> ProcessRef::image_path() const {
    std::wstring buffer(32768, L'\0');
    DWORD size = static_cast<DWORD>(buffer.size());
    if (!QueryFullProcessImageNameW(handle_.get(), 0, buffer.data(), &size)) return std::nullopt;
    buffer.resize(size);
    return std::filesystem::path(buffer);
}

std::optional<std::uint64_t> ProcessRef::creation_ticks() const {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(handle_.get(), &created, &exited, &kernel, &user)) return std::nullopt;
    return ticks_of(created);
}

std::uint32_t ProcessRef::session_id() const {
    DWORD session = 0;
    ProcessIdToSessionId(pid_, &session);
    return session;
}

bool ProcessRef::terminate() const {
    return TerminateProcess(handle_.get(), 1) != FALSE;
}

std::vector<std::uint32_t> running_process_ids() {
    std::vector<std::uint32_t> ids;
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot.get() == INVALID_HANDLE_VALUE) return ids;
    PROCESSENTRY32W entry{sizeof(entry)};
    for (BOOL ok = Process32FirstW(snapshot.get(), &entry); ok; ok = Process32NextW(snapshot.get(), &entry)) {
        ids.push_back(entry.th32ProcessID);
    }
    return ids;
}

}  // namespace houseguest::system
