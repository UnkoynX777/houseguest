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
#include <fstream>
#include <memory>
#include <set>
#include <sstream>

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

std::string version_string(const std::filesystem::path& file, const wchar_t* field) {
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
    wchar_t query[96];
    swprintf_s(query, L"\\StringFileInfo\\%04x%04x\\%s", translations[0].language, translations[0].code_page,
               field);
    wchar_t* company = nullptr;
    if (!VerQueryValueW(block.data(), query, reinterpret_cast<void**>(&company), &length) ||
        company == nullptr) {
        return {};
    }
    return to_utf8(company);
}

std::string company_name(const std::filesystem::path& file) {
    return version_string(file, L"CompanyName");
}

InstalledApp make_app(std::string name, const std::filesystem::path& target, const char* source) {
    InstalledApp app;
    app.name = std::move(name);
    app.path = path_to_utf8(target);
    app.company = company_name(target);
    app.version = version_string(target, L"FileVersion");
    app.description = version_string(target, L"FileDescription");
    app.source = source;
    return app;
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
        if (!is_local_disk_path(target) || !std::filesystem::exists(target, ec)) continue;
        const auto name = it->path().stem().string();
        const auto lowered = lower(name);
        if (lowered.find("uninstall") != std::string::npos) continue;
        const auto path = path_to_utf8(target);
        if (!seen.insert(lower(path)).second) continue;
        out.push_back(make_app(to_utf8(it->path().stem().wstring()), target, "start_menu"));
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
        if (!is_local_disk_path(target) || !std::filesystem::is_regular_file(target, ec)) continue;
        const auto path = path_to_utf8(target);
        if (!seen.insert(lower(path)).second) continue;
        out.push_back(make_app(to_utf8(target.stem().wstring()), target, "app_paths"));
    }
    RegCloseKey(root);
}

std::wstring registry_text(HKEY root, const std::wstring& key, const wchar_t* value) {
    wchar_t buffer[MAX_PATH * 2] = {};
    DWORD size = sizeof(buffer) - sizeof(wchar_t);
    if (RegGetValueW(root, key.c_str(), value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, buffer,
                     &size) != ERROR_SUCCESS) {
        return {};
    }
    return buffer;
}

std::filesystem::path executable_from_icon(std::wstring text) {
    if (const auto comma = text.rfind(L','); comma != std::wstring::npos) text.resize(comma);
    text.erase(std::remove(text.begin(), text.end(), L'"'), text.end());
    wchar_t expanded[MAX_PATH * 2] = {};
    ExpandEnvironmentStringsW(text.c_str(), expanded, MAX_PATH * 2);
    return expanded;
}

void scan_uninstall(REGSAM view, std::set<std::string>& seen, std::vector<InstalledApp>& out) {
    const std::wstring parent = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, parent.c_str(), 0, KEY_READ | view, &root) != ERROR_SUCCESS) return;
    wchar_t name[256];
    for (DWORD index = 0;; ++index) {
        DWORD name_size = 256;
        if (RegEnumKeyExW(root, index, name, &name_size, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;
        const auto key = parent + L"\\" + name;
        DWORD hidden = 0, hidden_size = sizeof(hidden);
        RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"SystemComponent", RRF_RT_REG_DWORD | view, nullptr,
                     &hidden, &hidden_size);
        const auto display = registry_text(HKEY_LOCAL_MACHINE, key, L"DisplayName");
        if (hidden != 0 || display.empty()) continue;
        const auto icon = registry_text(HKEY_LOCAL_MACHINE, key, L"DisplayIcon");
        if (icon.empty()) continue;
        const auto target = executable_from_icon(icon);
        std::error_code ec;
        if (lower(target.extension().string()) != ".exe" || !is_local_disk_path(target) ||
            !std::filesystem::is_regular_file(target, ec)) {
            continue;
        }
        if (lower(target.filename().string()).starts_with("unins")) continue;
        if (!seen.insert(lower(path_to_utf8(target))).second) continue;
        out.push_back(make_app(to_utf8(display), target, "installed"));
    }
    RegCloseKey(root);
}

std::string xml_attribute(const std::string& tag, const std::string& attribute) {
    const auto at = tag.find(attribute + "=\"");
    if (at == std::string::npos) return {};
    const auto begin = at + attribute.size() + 2;
    const auto end = tag.find('"', begin);
    return end == std::string::npos ? std::string{} : tag.substr(begin, end - begin);
}

std::string xml_element(const std::string& xml, const std::string& element) {
    const auto open = xml.find("<" + element + ">");
    if (open == std::string::npos) return {};
    const auto begin = open + element.size() + 2;
    const auto end = xml.find("</" + element + ">", begin);
    return end == std::string::npos ? std::string{} : xml.substr(begin, end - begin);
}

std::string humanize(std::string identity) {
    if (const auto dot = identity.rfind('.'); dot != std::string::npos) identity = identity.substr(dot + 1);
    std::string out;
    for (std::size_t i = 0; i < identity.size(); ++i) {
        if (i > 0 && std::isupper(static_cast<unsigned char>(identity[i])) &&
            !std::isupper(static_cast<unsigned char>(identity[i - 1])))
            out += ' ';
        out += identity[i];
    }
    return out;
}

void scan_packages(std::set<std::string>& seen, std::vector<InstalledApp>& out) {
    const std::wstring parent =
        L"SOFTWARE\\Classes\\Local "
        L"Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppModel\\Repository\\"
        L"Packages";
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, parent.c_str(), 0, KEY_READ, &root) != ERROR_SUCCESS) return;
    wchar_t full_name[256];
    for (DWORD index = 0;; ++index) {
        DWORD size = 256;
        if (RegEnumKeyExW(root, index, full_name, &size, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;
        const std::filesystem::path location =
            registry_text(HKEY_LOCAL_MACHINE, parent + L"\\" + full_name, L"Path");
        std::error_code ec;
        if (location.empty() || lower(location.string()).find("\\systemapps\\") != std::string::npos)
            continue;
        std::ifstream manifest_file(location / L"AppxManifest.xml", std::ios::binary);
        if (!manifest_file) continue;
        std::ostringstream buffer;
        buffer << manifest_file.rdbuf();
        const auto manifest = buffer.str();
        if (manifest.size() > (1u << 20) || xml_element(manifest, "Framework") == "true") continue;

        const auto application_at = manifest.find("<Application ");
        if (application_at == std::string::npos) continue;
        const auto application =
            manifest.substr(application_at, manifest.find('>', application_at) - application_at);
        const auto executable = xml_attribute(application, "Executable");
        if (executable.empty() || lower(executable).find(".exe") == std::string::npos) continue;
        const auto target = location / path_from_utf8(executable);
        if (!std::filesystem::is_regular_file(target, ec) || !seen.insert(lower(path_to_utf8(target))).second)
            continue;

        const auto identity_at = manifest.find("<Identity ");
        const auto identity =
            identity_at == std::string::npos
                ? std::string{}
                : manifest.substr(identity_at, manifest.find('>', identity_at) - identity_at);
        const auto package_name = xml_attribute(identity, "Name");
        auto display = xml_element(manifest, "DisplayName");
        if (display.starts_with("ms-resource:")) {
            const auto resource = std::wstring(L"@{") + full_name + L"? ms-resource://" +
                                  to_wide(package_name) + L"/resources/" + to_wide(display.substr(12)) + L"}";
            wchar_t resolved[256] = {};
            display =
                SUCCEEDED(SHLoadIndirectString(resource.c_str(), resolved, 256, nullptr)) && resolved[0] != 0
                    ? to_utf8(resolved)
                    : humanize(package_name);
        }
        if (display.empty()) display = humanize(package_name);

        auto app = make_app(display, target, "store");
        app.company = xml_element(manifest, "PublisherDisplayName");
        if (app.company.starts_with("ms-resource:")) app.company.clear();
        UINT32 family_length = 256;
        wchar_t family[256] = {};
        if (PackageFamilyNameFromFullName(full_name, &family_length, family) == ERROR_SUCCESS)
            app.package_family = to_utf8(family);
        out.push_back(std::move(app));
    }
    RegCloseKey(root);
}

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
    scan_uninstall(KEY_WOW64_64KEY, seen, apps);
    scan_uninstall(KEY_WOW64_32KEY, seen, apps);
    scan_packages(seen, apps);
    if (initialized) CoUninitialize();

    std::ranges::sort(
        apps, [](const InstalledApp& a, const InstalledApp& b) { return lower(a.name) < lower(b.name); });
    return apps;
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

InstalledApp describe_executable(const std::filesystem::path& file) {
    return make_app(path_to_utf8(file.stem()), file, "manual");
}

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
