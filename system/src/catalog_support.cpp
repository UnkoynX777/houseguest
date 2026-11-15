#include "catalog_support.hpp"

#include <cwctype>
#include <shobjidl.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cctype>
#include <deque>
#include <fstream>
#include <set>

#include "houseguest/system/text.hpp"
#include "houseguest/system/windows_info.hpp"

namespace houseguest::system::catalog {

std::string lower(std::string value) {
    std::ranges::transform(value, value.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string alnum(const std::string& text) {
    std::string out;
    for (const unsigned char c : text) {
        if (std::isalnum(c)) out += static_cast<char>(std::tolower(c));
    }
    return out;
}

std::vector<std::string> tokens(const std::string& text) {
    static const std::set<std::string> filler = {"the", "and",   "for",   "of",      "x64", "x86",
                                                 "bit", "64bit", "32bit", "edition", "app", "user"};
    std::vector<std::string> out;
    std::string current;
    const auto flush = [&] {
        if (!current.empty() && !filler.contains(current)) out.push_back(current);
        current.clear();
    };
    for (const unsigned char c : text) {
        if (std::isalnum(c))
            current += static_cast<char>(std::tolower(c));
        else
            flush();
    }
    flush();
    return out;
}

double token_overlap(const std::vector<std::string>& wanted, const std::vector<std::string>& offered) {
    std::size_t total = 0, matched = 0;
    for (const auto& token : wanted) {
        total += token.size();
        if (std::ranges::find(offered, token) != offered.end()) matched += token.size();
    }
    return total == 0 ? 0.0 : static_cast<double>(matched) / static_cast<double>(total);
}

fs::path expand(std::wstring text) {
    text.erase(std::remove(text.begin(), text.end(), L'"'), text.end());
    wchar_t expanded[MAX_PATH * 2] = {};
    const auto length = ExpandEnvironmentStringsW(text.c_str(), expanded, MAX_PATH * 2);
    fs::path path = length == 0 || length > MAX_PATH * 2 ? fs::path(text) : fs::path(expanded);
    return path.lexically_normal().make_preferred();
}

std::optional<IconLocation> parse_icon_location(std::wstring text) {
    while (!text.empty() && (text.front() == L' ' || text.front() == L'"')) text.erase(text.begin());
    int index = 0;
    if (const auto comma = text.rfind(L','); comma != std::wstring::npos) {
        const auto tail = text.substr(comma + 1);
        const auto digits = tail.find_first_of(L"0123456789");
        const bool numeric = !tail.empty() && tail.find_first_not_of(L" -0123456789\"") == std::wstring::npos &&
                             digits != std::wstring::npos;
        if (numeric) {
            index = _wtoi(tail.c_str());
            text.resize(comma);
        } else if (tail.find_first_not_of(L" \"") == std::wstring::npos) {
            text.resize(comma);
        }
    }
    while (!text.empty() && (text.back() == L'"' || text.back() == L' ')) text.pop_back();
    if (text.empty() || text.size() > 1024) return std::nullopt;
    auto file = expand(text);
    if (file.empty() || !file.is_absolute() || !is_local_disk_path(file)) return std::nullopt;
    return IconLocation{std::move(file), index};
}

fs::path environment_path(const wchar_t* name) {
    wchar_t buffer[MAX_PATH * 2] = {};
    const auto length = GetEnvironmentVariableW(name, buffer, MAX_PATH * 2);
    return length == 0 || length >= MAX_PATH * 2 ? fs::path() : fs::path(buffer);
}

namespace {
DWORD view_flag(REGSAM view) {
    return view == KEY_WOW64_32KEY   ? RRF_SUBKEY_WOW6432KEY
           : view == KEY_WOW64_64KEY ? RRF_SUBKEY_WOW6464KEY
                                     : 0;
}
}  // namespace

std::wstring registry_text(HKEY root, const std::wstring& key, const wchar_t* value, REGSAM view) {
    wchar_t buffer[MAX_PATH * 4] = {};
    DWORD size = sizeof(buffer) - sizeof(wchar_t);
    if (RegGetValueW(root, key.c_str(), value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | view_flag(view),
                     nullptr, buffer, &size) != ERROR_SUCCESS)
        return {};
    return buffer;
}

DWORD registry_number(HKEY root, const std::wstring& key, const wchar_t* value, REGSAM view) {
    DWORD number = 0, size = sizeof(number);
    RegGetValueW(root, key.c_str(), value, RRF_RT_REG_DWORD | view_flag(view), nullptr, &number, &size);
    return number;
}

std::vector<std::wstring> subkeys(HKEY root, const std::wstring& key, REGSAM view) {
    std::vector<std::wstring> names;
    HKEY handle = nullptr;
    if (RegOpenKeyExW(root, key.c_str(), 0, KEY_READ | view, &handle) != ERROR_SUCCESS) return names;
    wchar_t name[256];
    for (DWORD index = 0; names.size() < 8000; ++index) {
        DWORD size = 256;
        if (RegEnumKeyExW(handle, index, name, &size, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;
        names.emplace_back(name, size);
    }
    RegCloseKey(handle);
    return names;
}

fs::path resolve_shortcut(const fs::path& link) { return read_shortcut(link).target; }

ShortcutTarget read_shortcut(const fs::path& link) {
    IShellLinkW* shell_link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                reinterpret_cast<void**>(&shell_link))))
        return {};
    ShortcutTarget result;
    fs::path& target = result.target;
    IPersistFile* file = nullptr;
    if (SUCCEEDED(shell_link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file)))) {
        if (SUCCEEDED(file->Load(link.c_str(), STGM_READ))) {
            wchar_t buffer[MAX_PATH * 2] = {};
            if (SUCCEEDED(shell_link->GetPath(buffer, MAX_PATH * 2, nullptr, SLGP_RAWPATH)))
                target = expand(buffer);
            wchar_t icon[MAX_PATH * 2] = {};
            int index = 0;
            if (SUCCEEDED(shell_link->GetIconLocation(icon, MAX_PATH * 2, &index)) && icon[0] != 0) {
                if (auto location = parse_icon_location(icon)) {
                    location->index = index;
                    result.icon = std::move(location);
                }
            }
        }
        file->Release();
    }
    shell_link->Release();
    return result;
}

VersionInfo read_version(const fs::path& file) {
    VersionInfo info;
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(file.c_str(), &ignored);
    if (size == 0 || size > (16u << 20)) return info;
    std::vector<std::uint8_t> block(size);
    if (!GetFileVersionInfoW(file.c_str(), 0, size, block.data())) return info;

    // Programs declare their strings under one or more language and code page pairs, and some declare none in
    // the translation table at all, so the common pairs are tried after the declared ones.
    struct Translation {
        WORD language;
        WORD code_page;
    };
    std::vector<Translation> translations;
    Translation* declared = nullptr;
    UINT length = 0;
    if (VerQueryValueW(block.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&declared), &length) &&
        declared != nullptr) {
        for (UINT i = 0; i < std::min<UINT>(length / sizeof(Translation), 8); ++i) translations.push_back(declared[i]);
    }
    for (const Translation common : {Translation{0x0409, 0x04B0}, Translation{0x0409, 0x04E4}, Translation{0x0000, 0x04B0}})
        translations.push_back(common);

    const auto field = [&](const wchar_t* name) -> std::string {
        for (const auto& translation : translations) {
            wchar_t query[96];
            swprintf_s(query, L"\\StringFileInfo\\%04x%04x\\%s", translation.language, translation.code_page, name);
            wchar_t* value = nullptr;
            UINT size_out = 0;
            if (!VerQueryValueW(block.data(), query, reinterpret_cast<void**>(&value), &size_out) || value == nullptr ||
                size_out == 0)
                continue;
            std::wstring text(value, wcsnlen(value, 512));
            while (!text.empty() && std::iswspace(text.back())) text.pop_back();
            text.erase(0, std::ranges::find_if_not(text, [](wchar_t c) { return std::iswspace(c); }) - text.begin());
            if (!text.empty()) return to_utf8(text);
        }
        return {};
    };
    info.company = field(L"CompanyName");
    info.product = field(L"ProductName");
    info.description = field(L"FileDescription");
    info.version = field(L"FileVersion");
    info.product_version = field(L"ProductVersion");
    info.copyright = field(L"LegalCopyright");
    info.original_name = field(L"OriginalFilename");
    info.internal_name = field(L"InternalName");
    if (info.version.empty()) {
        VS_FIXEDFILEINFO* fixed = nullptr;
        UINT fixed_size = 0;
        if (VerQueryValueW(block.data(), L"\\", reinterpret_cast<void**>(&fixed), &fixed_size) && fixed != nullptr &&
            fixed_size >= sizeof(VS_FIXEDFILEINFO) && fixed->dwSignature == 0xFEEF04BD && fixed->dwFileVersionMS != 0) {
            info.version = std::to_string(HIWORD(fixed->dwFileVersionMS)) + "." + std::to_string(LOWORD(fixed->dwFileVersionMS)) +
                           "." + std::to_string(HIWORD(fixed->dwFileVersionLS)) + "." + std::to_string(LOWORD(fixed->dwFileVersionLS));
        }
    }
    return info;
}

}  // namespace houseguest::system::catalog

namespace houseguest::system {

FileMetadata read_file_metadata(const std::filesystem::path& file) {
    const auto info = catalog::read_version(file);
    return {info.company, info.product, info.description, info.version, info.product_version,
            info.copyright, info.original_name, info.internal_name};
}

}  // namespace houseguest::system

namespace houseguest::system::catalog {

Subsystem pe_subsystem(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return Subsystem::Unknown;
    IMAGE_DOS_HEADER dos{};
    in.read(reinterpret_cast<char*>(&dos), sizeof(dos));
    if (!in || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || dos.e_lfanew > 4096)
        return Subsystem::Unknown;
    in.seekg(dos.e_lfanew);
    DWORD signature = 0;
    IMAGE_FILE_HEADER header{};
    in.read(reinterpret_cast<char*>(&signature), sizeof(signature));
    in.read(reinterpret_cast<char*>(&header), sizeof(header));
    std::uint8_t optional[72] = {};
    in.read(reinterpret_cast<char*>(optional), sizeof(optional));
    if (!in || signature != IMAGE_NT_SIGNATURE) return Subsystem::Unknown;
    const WORD subsystem = static_cast<WORD>(optional[68] | (optional[69] << 8));
    if (subsystem == IMAGE_SUBSYSTEM_WINDOWS_GUI) return Subsystem::Gui;
    if (subsystem == IMAGE_SUBSYSTEM_WINDOWS_CUI) return Subsystem::Console;
    return Subsystem::Unknown;
}

std::string canonical_key(const fs::path& path) {
    wchar_t buffer[MAX_PATH * 2] = {};
    const auto length = GetLongPathNameW(path.c_str(), buffer, MAX_PATH * 2);
    auto text = to_utf8(length != 0 && length < MAX_PATH * 2 ? std::wstring(buffer) : path.wstring());
    std::ranges::replace(text, '/', '\\');
    while (text.size() > 3 && text.back() == '\\') text.pop_back();
    return lower(std::move(text));
}

bool key_inside(const std::string& root_key, const std::string& key) {
    return key.size() > root_key.size() && key.starts_with(root_key) &&
           (root_key.back() == '\\' || key[root_key.size()] == '\\');
}

bool under_windows(const std::string& lowered_path) {
    static const std::string windows = [] {
        wchar_t buffer[MAX_PATH] = {};
        GetWindowsDirectoryW(buffer, MAX_PATH);
        return lower(to_utf8(buffer)) + "\\";
    }();
    return lowered_path.starts_with(windows);
}

bool usable_executable(const fs::path& file) {
    std::error_code ec;
    return lower(file.extension().string()) == ".exe" && is_local_disk_path(file) &&
           fs::is_regular_file(file, ec);
}

std::string volume_of(const fs::path& path) {
    const auto text = path.string();
    if (text.size() >= 2 && text[1] == ':')
        return std::string{static_cast<char>(std::toupper(static_cast<unsigned char>(text[0]))), ':'};
    return {};
}

namespace {

std::wstring long_form(const std::wstring& path) {
    return path.size() > 240 && !path.starts_with(L"\\\\?\\") ? L"\\\\?\\" + path : path;
}

std::int64_t file_time_epoch(const FILETIME& time) {
    ULARGE_INTEGER value;
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return static_cast<std::int64_t>(value.QuadPart / 10000000ULL) - 11644473600LL;
}

}  // namespace

WalkResult find_executables(const fs::path& root, const WalkBudget& budget,
                            const std::function<bool(const std::wstring& name)>& skip_directory) {
    WalkResult result;
    const auto started = std::chrono::steady_clock::now();
    std::deque<std::wstring> queue{root.wstring()};
    while (!queue.empty()) {
        if ((budget.cancel != nullptr && budget.cancel->load(std::memory_order_relaxed)) ||
            result.visited >= budget.max_entries ||
            std::chrono::steady_clock::now() - started > budget.max_time) {
            result.truncated = true;
            break;
        }
        const auto directory = std::move(queue.front());
        queue.pop_front();
        WIN32_FIND_DATAW data;
        const HANDLE handle = FindFirstFileExW(long_form(directory + L"\\*").c_str(), FindExInfoBasic, &data,
                                               FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (handle == INVALID_HANDLE_VALUE) continue;
        do {
            const std::wstring name = data.cFileName;
            if (name == L"." || name == L"..") continue;
            ++result.visited;
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                    ++result.skipped_links;
                    continue;
                }
                if (!skip_directory || !skip_directory(name)) queue.push_back(directory + L"\\" + name);
                continue;
            }
            if (name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".exe") == 0) {
                ULARGE_INTEGER size;
                size.LowPart = data.nFileSizeLow;
                size.HighPart = data.nFileSizeHigh;
                result.executables.push_back({fs::path(directory + L"\\" + name), size.QuadPart,
                                              file_time_epoch(data.ftLastWriteTime)});
            }
        } while (FindNextFileW(handle, &data));
        FindClose(handle);
    }
    return result;
}

std::vector<std::string> running_images() {
    std::vector<std::string> images;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return images;
    PROCESSENTRY32W entry{sizeof(entry)};
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
        const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (process == nullptr) continue;
        wchar_t path[MAX_PATH * 2] = {};
        DWORD size = MAX_PATH * 2;
        if (QueryFullProcessImageNameW(process, 0, path, &size)) images.push_back(canonical_key(path));
        CloseHandle(process);
    }
    CloseHandle(snapshot);
    std::ranges::sort(images);
    images.erase(std::unique(images.begin(), images.end()), images.end());
    return images;
}

std::string hash_text(const std::string& text) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char c : text) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    char buffer[17];
    snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(hash));
    return buffer;
}

std::int64_t epoch_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace houseguest::system::catalog
