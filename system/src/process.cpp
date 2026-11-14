#include "houseguest/system/process.hpp"

#include <windows.h>

#include <appmodel.h>
#include <tlhelp32.h>

#include "win_util.hpp"

namespace houseguest::system {
namespace {

constexpr DWORD kQuery = PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE;
constexpr DWORD kAccess = kQuery | PROCESS_TERMINATE;

std::uint64_t ticks_of(const FILETIME& time) {
    return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

}  // namespace

std::optional<ProcessRef> ProcessRef::open(std::uint32_t pid, std::uint32_t* error) {
    HANDLE raw = OpenProcess(kAccess, FALSE, pid);
    if (!raw) raw = OpenProcess(kQuery, FALSE, pid);
    if (!raw) raw = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!raw) {
        if (error) *error = GetLastError();
        return std::nullopt;
    }
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

std::string ProcessRef::package_family() const {
    UINT32 length = 0;
    if (GetPackageFamilyName(handle_.get(), &length, nullptr) != ERROR_INSUFFICIENT_BUFFER || length == 0)
        return {};
    std::wstring buffer(length, L'\0');
    if (GetPackageFamilyName(handle_.get(), &length, buffer.data()) != ERROR_SUCCESS || length == 0)
        return {};
    buffer.resize(length - 1);
    return to_utf8(buffer);
}

bool ProcessRef::exited() const {
    return WaitForSingleObject(handle_.get(), 0) == WAIT_OBJECT_0;
}

std::optional<std::uint64_t> ProcessRef::exit_ticks() const {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(handle_.get(), &created, &exited, &kernel, &user)) return std::nullopt;
    const auto ticks = ticks_of(exited);
    if (ticks == 0) return std::nullopt;
    return ticks;
}

bool ProcessRef::kernel_critical() const {
    BOOL critical = FALSE;
    return IsProcessCritical(handle_.get(), &critical) && critical;
}

bool ProcessRef::protected_process() const {
    PROCESS_PROTECTION_LEVEL_INFORMATION level{};
    if (!GetProcessInformation(handle_.get(), ProcessProtectionLevelInfo, &level, sizeof(level)))
        return false;
    return level.ProtectionLevel != PROTECTION_LEVEL_NONE;
}

ProcessRef::Termination ProcessRef::terminate_checked() const {
    if (kernel_critical() || protected_process()) return Termination::Refused;
    return TerminateProcess(handle_.get(), 1) ? Termination::Done : Termination::Failed;
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

namespace houseguest::system {

ProcessSnapshot ProcessSnapshot::take() {
    ProcessSnapshot snapshot;
    snapshot.taken_ = std::chrono::steady_clock::now();
    Handle handle(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (handle.get() == INVALID_HANDLE_VALUE) return snapshot;
    PROCESSENTRY32W entry{sizeof(entry)};
    for (BOOL ok = Process32FirstW(handle.get(), &entry); ok; ok = Process32NextW(handle.get(), &entry)) {
        snapshot.parents_[entry.th32ProcessID] = entry.th32ParentProcessID;
    }
    return snapshot;
}

std::optional<std::uint32_t> ProcessSnapshot::parent_of(std::uint32_t pid) const {
    const auto it = parents_.find(pid);
    if (it == parents_.end()) return std::nullopt;
    return it->second;
}

}  // namespace houseguest::system
