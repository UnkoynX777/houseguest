#include "houseguest/system/windows_info.hpp"

// clang-format off
// mscat.h requires wincrypt.h to be included first.
#include <windows.h>
#include <bcrypt.h>
#include <lm.h>
#include <sddl.h>
#include <shobjidl.h>
#include <softpub.h>
#include <wincrypt.h>
#include <mscat.h>
#include <wintrust.h>
#include <wtsapi32.h>
// clang-format on

#include <algorithm>
#include <cctype>
#include <memory>
#include <set>

#include "houseguest/system/text.hpp"

#include "win_util.hpp"

namespace houseguest::system {
namespace {

std::string lower(std::string value) {
    std::ranges::transform(value, value.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::filesystem::path resolve_shortcut(const std::filesystem::path& link) {
    IShellLinkW* shell_link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                reinterpret_cast<void**>(&shell_link)))) {
        return {};
    }
    std::filesystem::path target;
    IPersistFile* file = nullptr;
    if (SUCCEEDED(shell_link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file)))) {
        if (SUCCEEDED(file->Load(link.c_str(), STGM_READ))) {
            wchar_t buffer[MAX_PATH * 2] = {};
            if (SUCCEEDED(shell_link->GetPath(buffer, MAX_PATH * 2, nullptr, SLGP_RAWPATH))) {
                target = buffer;
            }
        }
        file->Release();
    }
    shell_link->Release();
    return target;
}

std::string company_name(const std::filesystem::path& file) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(file.c_str(), &ignored);
    if (size == 0) return {};
    std::vector<std::uint8_t> block(size);
    if (!GetFileVersionInfoW(file.c_str(), 0, size, block.data())) return {};

    struct Translation {
        WORD language;
        WORD code_page;
    };
    Translation* translations = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(block.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translations),
                        &length) ||
        length < sizeof(Translation)) {
        return {};
    }
    wchar_t query[64];
    swprintf_s(query, L"\\StringFileInfo\\%04x%04x\\CompanyName", translations[0].language,
               translations[0].code_page);
    wchar_t* company = nullptr;
    if (!VerQueryValueW(block.data(), query, reinterpret_cast<void**>(&company), &length) ||
        company == nullptr) {
        return {};
    }
    return to_utf8(company);
}

void scan_start_menu(const std::filesystem::path& root, std::set<std::string>& seen,
                     std::vector<InstalledApp>& out) {
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) return;
    for (std::filesystem::recursive_directory_iterator
             it(root, std::filesystem::directory_options::skip_permission_denied, ec),
         end;
         it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec) || lower(it->path().extension().string()) != ".lnk") continue;
        const auto target = resolve_shortcut(it->path());
        if (target.empty() || lower(target.extension().string()) != ".exe") continue;
        if (!std::filesystem::exists(target, ec)) continue;
        const auto name = it->path().stem().string();
        const auto lowered = lower(name);
        if (lowered.find("uninstall") != std::string::npos) continue;
        const auto path = path_to_utf8(target);
        if (!seen.insert(lower(path)).second) continue;
        out.push_back({to_utf8(it->path().stem().wstring()), path, company_name(target)});
    }
}

void scan_app_paths(std::set<std::string>& seen, std::vector<InstalledApp>& out) {
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths", 0,
                      KEY_READ, &root) != ERROR_SUCCESS) {
        return;
    }
    wchar_t name[256];
    for (DWORD index = 0;; ++index) {
        DWORD name_size = 256;
        if (RegEnumKeyExW(root, index, name, &name_size, nullptr, nullptr, nullptr, nullptr) !=
            ERROR_SUCCESS) {
            break;
        }
        wchar_t value[MAX_PATH * 2] = {};
        DWORD value_size = sizeof(value) - sizeof(wchar_t);
        if (RegGetValueW(root, name, nullptr, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, value,
                         &value_size) != ERROR_SUCCESS) {
            continue;
        }
        std::wstring raw(value);
        raw.erase(std::remove(raw.begin(), raw.end(), L'"'), raw.end());
        wchar_t expanded[MAX_PATH * 2] = {};
        ExpandEnvironmentStringsW(raw.c_str(), expanded, MAX_PATH * 2);
        std::filesystem::path target = expanded;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(target, ec)) continue;
        const auto path = path_to_utf8(target);
        if (!seen.insert(lower(path)).second) continue;
        out.push_back({to_utf8(target.stem().wstring()), path, company_name(target)});
    }
    RegCloseKey(root);
}

}  // namespace

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

namespace {

std::string verified_signer_subject(WINTRUST_DATA& data) {
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const auto window = static_cast<HWND>(INVALID_HANDLE_VALUE);
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;

    std::string subject;
    if (WinVerifyTrust(window, &action, &data) == ERROR_SUCCESS) {
        if (auto* provider = WTHelperProvDataFromStateData(data.hWVTStateData)) {
            if (auto* signer = WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0)) {
                if (auto* cert = WTHelperGetProvCertFromChain(signer, 0)) {
                    wchar_t name[512] = {};
                    CertNameToStrW(X509_ASN_ENCODING, &cert->pCert->pCertInfo->Subject, CERT_X500_NAME_STR,
                                   name, 512);
                    subject = to_utf8(name);
                }
            }
        }
    }
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(window, &action, &data);
    return subject;
}

std::string catalog_signer_subject(const std::filesystem::path& file) {
    Handle handle(CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (handle.get() == INVALID_HANDLE_VALUE) return {};

    for (const wchar_t* algorithm : {BCRYPT_SHA256_ALGORITHM, static_cast<const wchar_t*>(nullptr)}) {
        HCATADMIN admin = nullptr;
        if (!CryptCATAdminAcquireContext2(&admin, nullptr, algorithm, nullptr, 0)) continue;
        std::string subject;

        DWORD hash_size = 0;
        CryptCATAdminCalcHashFromFileHandle2(admin, handle.get(), &hash_size, nullptr, 0);
        std::vector<BYTE> hash(hash_size);
        if (hash_size != 0 &&
            CryptCATAdminCalcHashFromFileHandle2(admin, handle.get(), &hash_size, hash.data(), 0)) {
            if (HCATINFO catalog =
                    CryptCATAdminEnumCatalogFromHash(admin, hash.data(), hash_size, 0, nullptr)) {
                CATALOG_INFO info{sizeof(info)};
                if (CryptCATCatalogInfoFromContext(catalog, &info, 0)) {
                    std::wstring tag;
                    for (const BYTE byte : hash) {
                        wchar_t pair[3];
                        swprintf_s(pair, L"%02X", byte);
                        tag += pair;
                    }
                    WINTRUST_CATALOG_INFO catalog_info{};
                    catalog_info.cbStruct = sizeof(catalog_info);
                    catalog_info.pcwszCatalogFilePath = info.wszCatalogFile;
                    catalog_info.pcwszMemberTag = tag.c_str();
                    catalog_info.pcwszMemberFilePath = file.c_str();
                    catalog_info.hMemberFile = handle.get();
                    catalog_info.pbCalculatedFileHash = hash.data();
                    catalog_info.cbCalculatedFileHash = hash_size;
                    catalog_info.hCatAdmin = admin;
                    WINTRUST_DATA data{};
                    data.dwUnionChoice = WTD_CHOICE_CATALOG;
                    data.pCatalog = &catalog_info;
                    subject = verified_signer_subject(data);
                }
                CryptCATAdminReleaseCatalogContext(admin, catalog, 0);
            }
        }
        CryptCATAdminReleaseContext(admin, 0);
        if (!subject.empty()) return subject;
    }
    return {};
}

}  // namespace

std::string trusted_publisher(const std::filesystem::path& file) {
    WINTRUST_FILE_INFO file_info{};
    file_info.cbStruct = sizeof(file_info);
    file_info.pcwszFilePath = file.c_str();
    WINTRUST_DATA data{};
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file_info;
    auto subject = verified_signer_subject(data);
    return subject.empty() ? catalog_signer_subject(file) : subject;
}

std::vector<InstalledApp> discover_installed_apps() {
    const bool initialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    std::set<std::string> seen;
    std::vector<InstalledApp> apps;

    wchar_t program_data[MAX_PATH] = {};
    GetEnvironmentVariableW(L"ProgramData", program_data, MAX_PATH);
    scan_start_menu(std::filesystem::path(program_data) / L"Microsoft\\Windows\\Start Menu\\Programs", seen,
                    apps);

    std::error_code ec;
    wchar_t drive[MAX_PATH] = {};
    GetEnvironmentVariableW(L"SystemDrive", drive, MAX_PATH);
    for (const auto& user : std::filesystem::directory_iterator(
             std::filesystem::path(drive) / L"\\Users",
             std::filesystem::directory_options::skip_permission_denied, ec)) {
        scan_start_menu(user.path() / L"AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs", seen,
                        apps);
    }
    scan_app_paths(seen, apps);
    if (initialized) CoUninitialize();

    std::ranges::sort(
        apps, [](const InstalledApp& a, const InstalledApp& b) { return lower(a.name) < lower(b.name); });
    return apps;
}

void disconnect_sessions(const std::vector<std::string>& user_sids, bool logoff) {
    WTS_SESSION_INFOW* sessions = nullptr;
    DWORD count = 0;
    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count)) return;
    for (DWORD i = 0; i < count; ++i) {
        HANDLE raw = nullptr;
        if (!WTSQueryUserToken(sessions[i].SessionId, &raw)) continue;
        Handle token(raw);
        const auto sid = token_user_sid(token.get());
        if (!sid || std::ranges::find(user_sids, *sid) == user_sids.end()) continue;
        if (logoff)
            WTSLogoffSession(WTS_CURRENT_SERVER_HANDLE, sessions[i].SessionId, FALSE);
        else
            WTSDisconnectSession(WTS_CURRENT_SERVER_HANDLE, sessions[i].SessionId, FALSE);
    }
    WTSFreeMemory(sessions);
}

void notify_session(std::uint32_t session_id, const std::wstring& title, const std::wstring& message) {
    DWORD response = 0;
    WTSSendMessageW(WTS_CURRENT_SERVER_HANDLE, session_id, const_cast<LPWSTR>(title.c_str()),
                    static_cast<DWORD>(title.size() * sizeof(wchar_t)), const_cast<LPWSTR>(message.c_str()),
                    static_cast<DWORD>(message.size() * sizeof(wchar_t)), MB_OK | MB_ICONINFORMATION, 0,
                    &response, FALSE);
}

}  // namespace houseguest::system
