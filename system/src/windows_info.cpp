#include "houseguest/system/windows_info.hpp"

// clang-format off
// mscat.h requires wincrypt.h to be included first.
#include <windows.h>
#include <aclapi.h>
#include <appmodel.h>
#include <bcrypt.h>
#include <lm.h>
#include <sddl.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <softpub.h>
#include <wincrypt.h>
#include <mscat.h>
#include <wintrust.h>
#include <wtsapi32.h>
// clang-format on

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>

#include "houseguest/system/text.hpp"

#include "win_util.hpp"

namespace houseguest::system {
namespace {

bool grants_write(const ACCESS_ALLOWED_ACE& ace) {
    constexpr DWORD kWrite = FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY | FILE_WRITE_DATA | FILE_APPEND_DATA |
                             FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER | GENERIC_WRITE |
                             GENERIC_ALL;
    return (ace.Mask & kWrite) != 0;
}

}  // namespace

bool is_local_disk_path(const std::filesystem::path& file) {
    const auto text = file.wstring();
    if (text.size() < 3 || text[1] != L':' || (text[2] != L'\\' && text[2] != L'/')) return false;
    if (!((text[0] >= L'A' && text[0] <= L'Z') || (text[0] >= L'a' && text[0] <= L'z'))) return false;
    const wchar_t root[] = {text[0], L':', L'\\', L'\0'};
    const UINT type = GetDriveTypeW(root);
    return type == DRIVE_FIXED || type == DRIVE_REMOVABLE || type == DRIVE_CDROM || type == DRIVE_RAMDISK;
}

std::vector<LocalUser> list_local_users() {
    std::vector<LocalUser> users;
    DWORD resume = 0;
    NET_API_STATUS status = NERR_Success;
    do {
        BYTE* buffer = nullptr;
        DWORD read = 0, total = 0;
        status = NetUserEnum(nullptr, 1, FILTER_NORMAL_ACCOUNT, &buffer, MAX_PREFERRED_LENGTH, &read, &total,
                             &resume);
        if (status != NERR_Success && status != ERROR_MORE_DATA) break;
        const auto* entries = reinterpret_cast<USER_INFO_1*>(buffer);
        for (DWORD i = 0; i < read; ++i) {
            BYTE sid_buffer[SECURITY_MAX_SID_SIZE];
            DWORD sid_size = sizeof(sid_buffer);
            wchar_t domain[256];
            DWORD domain_size = 256;
            SID_NAME_USE use = SidTypeUnknown;
            if (!LookupAccountNameW(nullptr, entries[i].usri1_name, sid_buffer, &sid_size, domain,
                                    &domain_size, &use)) {
                continue;
            }
            if (auto sid = sid_string(sid_buffer)) {
                users.push_back({to_utf8(entries[i].usri1_name), *sid,
                                 (entries[i].usri1_flags & UF_ACCOUNTDISABLE) != 0});
            }
        }
        NetApiBufferFree(buffer);
    } while (status == ERROR_MORE_DATA);
    return users;
}

std::optional<FileStamp> file_stamp(const std::filesystem::path& file) {
    Handle handle(CreateFileW(file.c_str(), FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (handle.get() == INVALID_HANDLE_VALUE) return std::nullopt;
    FILE_BASIC_INFO basic{};
    FILE_STANDARD_INFO standard{};
    if (!GetFileInformationByHandleEx(handle.get(), FileBasicInfo, &basic, sizeof(basic)) ||
        !GetFileInformationByHandleEx(handle.get(), FileStandardInfo, &standard, sizeof(standard)))
        return std::nullopt;
    FileStamp stamp;
    stamp.size = static_cast<std::uint64_t>(standard.EndOfFile.QuadPart);
    stamp.written = basic.LastWriteTime.QuadPart;
    stamp.changed = basic.ChangeTime.QuadPart;
    FILE_ID_INFO id{};
    if (GetFileInformationByHandleEx(handle.get(), FileIdInfo, &id, sizeof(id))) {
        stamp.volume = id.VolumeSerialNumber;
        std::memcpy(&stamp.id_low, id.FileId.Identifier, sizeof(stamp.id_low));
        std::memcpy(&stamp.id_high, id.FileId.Identifier + sizeof(stamp.id_low), sizeof(stamp.id_high));
    }
    return stamp;
}

SessionActionResult disconnect_sessions(const std::vector<std::string>& user_sids, bool logoff) {
    SessionActionResult result;
    WTS_SESSION_INFOW* sessions = nullptr;
    DWORD count = 0;
    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count)) {
        result.complete = false;
        return result;
    }
    for (DWORD i = 0; i < count; ++i) {
        HANDLE raw = nullptr;
        if (!WTSQueryUserToken(sessions[i].SessionId, &raw)) continue;
        Handle token(raw);
        const auto sid = token_user_sid(token.get());
        if (!sid || std::ranges::find(user_sids, *sid) == user_sids.end()) continue;
        const BOOL ok = logoff
                            ? WTSLogoffSession(WTS_CURRENT_SERVER_HANDLE, sessions[i].SessionId, FALSE)
                            : WTSDisconnectSession(WTS_CURRENT_SERVER_HANDLE, sessions[i].SessionId, FALSE);
        ok ? ++result.affected : (result.complete = false);
    }
    WTSFreeMemory(sessions);
    return result;
}

void notify_session(std::uint32_t session_id, const std::wstring& title, const std::wstring& message) {
    DWORD response = 0;
    WTSSendMessageW(WTS_CURRENT_SERVER_HANDLE, session_id, const_cast<LPWSTR>(title.c_str()),
                    static_cast<DWORD>(title.size() * sizeof(wchar_t)), const_cast<LPWSTR>(message.c_str()),
                    static_cast<DWORD>(message.size() * sizeof(wchar_t)), MB_OK | MB_ICONINFORMATION, 0,
                    &response, FALSE);
}

}  // namespace houseguest::system

namespace houseguest::system {

bool directory_writable_by(const std::filesystem::path& directory, const std::vector<std::string>& sids) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PSID owner = nullptr;
    PACL dacl = nullptr;
    if (GetNamedSecurityInfoW(directory.c_str(), SE_FILE_OBJECT,
                              OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr, &dacl,
                              nullptr, &descriptor) != ERROR_SUCCESS) {
        return true;
    }
    std::unique_ptr<void, LocalFreeDeleter> guard(descriptor);
    if (dacl == nullptr) return true;

    const auto matches = [&](PSID sid) {
        const auto text = sid_string(sid);
        if (!text) return true;
        static const std::set<std::string> broad = {"S-1-1-0", "S-1-5-32-545", "S-1-5-11", "S-1-5-4",
                                                    "S-1-5-32-546"};
        return broad.contains(*text) || std::ranges::find(sids, *text) != sids.end();
    };
    if (owner != nullptr && std::ranges::find(sids, sid_string(owner).value_or("")) != sids.end())
        return true;

    for (DWORD i = 0; i < dacl->AceCount; ++i) {
        void* raw = nullptr;
        if (!GetAce(dacl, i, &raw)) return true;
        const auto* header = static_cast<ACE_HEADER*>(raw);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) continue;
        auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw);
        if (grants_write(*ace) && matches(&ace->SidStart)) return true;
    }
    return false;
}

}  // namespace houseguest::system
