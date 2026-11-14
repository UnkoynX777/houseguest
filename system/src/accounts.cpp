#include "houseguest/system/accounts.hpp"

// clang-format off
#include <windows.h>
#include <lm.h>
#include <sddl.h>
// clang-format on

#include <algorithm>
#include <cctype>

#include "houseguest/system/text.hpp"

#include "win_util.hpp"

namespace houseguest::system {
namespace {

constexpr DWORD kMinimumManagedRid = 1000;

struct Resolved {
    std::wstring name;
    bool local_user = false;
};

Resolved resolve_local_user(const std::string& sid_text) {
    Resolved resolved;
    PSID sid = nullptr;
    if (!ConvertStringSidToSidW(to_wide(sid_text).c_str(), &sid)) return resolved;
    std::unique_ptr<void, LocalFreeDeleter> guard(sid);

    const auto count = *GetSidSubAuthorityCount(sid);
    if (count == 0 || *GetSidSubAuthority(sid, count - 1) < kMinimumManagedRid) return resolved;

    wchar_t name[256];
    wchar_t domain[256];
    DWORD name_size = 256;
    DWORD domain_size = 256;
    SID_NAME_USE use = SidTypeUnknown;
    if (!LookupAccountSidW(nullptr, sid, name, &name_size, domain, &domain_size, &use) || use != SidTypeUser)
        return resolved;

    wchar_t computer[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD computer_size = MAX_COMPUTERNAME_LENGTH + 1;
    if (!GetComputerNameW(computer, &computer_size) || _wcsicmp(computer, domain) != 0) return resolved;
    resolved.name = name;
    resolved.local_user = true;
    return resolved;
}

std::wstring users_group_name() {
    BYTE sid[SECURITY_MAX_SID_SIZE];
    DWORD size = sizeof(sid);
    if (!CreateWellKnownSid(WinBuiltinUsersSid, nullptr, sid, &size)) return L"Users";
    wchar_t name[256];
    wchar_t domain[256];
    DWORD name_size = 256;
    DWORD domain_size = 256;
    SID_NAME_USE use = SidTypeUnknown;
    if (!LookupAccountSidW(nullptr, sid, name, &name_size, domain, &domain_size, &use)) return L"Users";
    return name;
}

}  // namespace

bool valid_managed_name(const std::string& name) {
    if (name.empty() || name.size() > 16) return false;
    return std::ranges::all_of(name, [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_'; });
}

std::optional<CreatedUser> create_standard_user(const std::string& name, const std::string& password,
                                                std::string& error) {
    if (!valid_managed_name(name)) {
        error = "invalid name";
        return std::nullopt;
    }
    const auto wide_name = to_wide(name);
    auto wide_password = to_wide(password);

    USER_INFO_1 info{};
    info.usri1_name = const_cast<LPWSTR>(wide_name.c_str());
    info.usri1_password = wide_password.data();
    info.usri1_priv = USER_PRIV_USER;
    info.usri1_flags = UF_SCRIPT | UF_DONT_EXPIRE_PASSWD | UF_ACCOUNTDISABLE;
    DWORD parameter = 0;
    const NET_API_STATUS added = NetUserAdd(nullptr, 1, reinterpret_cast<BYTE*>(&info), &parameter);
    SecureZeroMemory(wide_password.data(), wide_password.size() * sizeof(wchar_t));
    if (added != NERR_Success) {
        error = added == NERR_UserExists ? "account already exists"
                                         : "NetUserAdd failed " + std::to_string(added);
        return std::nullopt;
    }

    BYTE sid[SECURITY_MAX_SID_SIZE];
    DWORD sid_size = sizeof(sid);
    wchar_t domain[256];
    DWORD domain_size = 256;
    SID_NAME_USE use = SidTypeUnknown;
    const auto text =
        LookupAccountNameW(nullptr, wide_name.c_str(), sid, &sid_size, domain, &domain_size, &use)
            ? sid_string(sid)
            : std::nullopt;
    if (!text) {
        NetUserDel(nullptr, wide_name.c_str());
        error = "cannot resolve the new account";
        return std::nullopt;
    }

    std::wstring qualified = std::wstring(domain) + L"\\" + wide_name;
    LOCALGROUP_MEMBERS_INFO_3 member{qualified.data()};
    const auto group = users_group_name();
    const NET_API_STATUS joined =
        NetLocalGroupAddMembers(nullptr, group.c_str(), 3, reinterpret_cast<BYTE*>(&member), 1);
    if (joined != NERR_Success && joined != ERROR_MEMBER_IN_ALIAS) {
        NetUserDel(nullptr, wide_name.c_str());
        error = "cannot add the account to the Users group";
        return std::nullopt;
    }
    return CreatedUser{name, *text};
}

bool set_user_disabled(const std::string& sid, bool disabled) {
    const auto user = resolve_local_user(sid);
    if (!user.local_user) return false;
    BYTE* buffer = nullptr;
    if (NetUserGetInfo(nullptr, user.name.c_str(), 1, &buffer) != NERR_Success) return false;
    DWORD flags = reinterpret_cast<USER_INFO_1*>(buffer)->usri1_flags;
    NetApiBufferFree(buffer);
    const bool currently_disabled = (flags & UF_ACCOUNTDISABLE) != 0;
    if (currently_disabled == disabled) return true;
    flags = disabled ? (flags | UF_ACCOUNTDISABLE) : (flags & ~static_cast<DWORD>(UF_ACCOUNTDISABLE));
    USER_INFO_1008 update{flags};
    DWORD parameter = 0;
    return NetUserSetInfo(nullptr, user.name.c_str(), 1008, reinterpret_cast<BYTE*>(&update), &parameter) ==
           NERR_Success;
}

bool set_user_expiry(const std::string& sid, std::int64_t unix_seconds) {
    const auto user = resolve_local_user(sid);
    if (!user.local_user) return false;
    USER_INFO_1017 update{unix_seconds <= 0 ? TIMEQ_FOREVER : static_cast<DWORD>(unix_seconds)};
    DWORD parameter = 0;
    return NetUserSetInfo(nullptr, user.name.c_str(), 1017, reinterpret_cast<BYTE*>(&update), &parameter) ==
           NERR_Success;
}

bool delete_user(const std::string& sid) {
    const auto user = resolve_local_user(sid);
    return user.local_user && NetUserDel(nullptr, user.name.c_str()) == NERR_Success;
}

bool user_exists(const std::string& sid) {
    return resolve_local_user(sid).local_user;
}

}  // namespace houseguest::system
