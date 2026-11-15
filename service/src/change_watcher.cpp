#include "change_watcher.hpp"

#include <windows.h>

#include <algorithm>
#include <memory>
#include <set>

#include "houseguest/system/app_catalog.hpp"
#include "houseguest/system/text.hpp"

namespace houseguest::service {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kMaxHandles = MAXIMUM_WAIT_OBJECTS - 2;
constexpr DWORD kFolderEvents = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME;

struct Place {
    std::string label;
    fs::path folder;
    bool subtree = false;
    DWORD filter = kFolderEvents;

    friend bool operator<(const Place& a, const Place& b) {
        return std::tie(a.folder, a.subtree) < std::tie(b.folder, b.subtree);
    }
};

struct Registered {
    std::string label;
    HANDLE handle = INVALID_HANDLE_VALUE;
    HKEY key = nullptr;
    bool registry = false;
};

std::wstring environment(const wchar_t* name) {
    wchar_t buffer[MAX_PATH] = {};
    const auto length = GetEnvironmentVariableW(name, buffer, MAX_PATH);
    return length > 0 && length < MAX_PATH ? std::wstring(buffer, length) : std::wstring{};
}

std::vector<Place> places(const ChangeWatcher::Roots& extra) {
    std::set<Place> found;
    const auto add = [&](std::string label, const fs::path& folder, bool subtree,
                         DWORD filter = kFolderEvents) {
        std::error_code ec;
        if (!folder.empty() && fs::is_directory(folder, ec))
            found.insert({std::move(label), folder, subtree, filter});
    };

    for (const auto* variable : {L"ProgramFiles", L"ProgramFiles(x86)", L"ProgramW6432"})
        add("program files", environment(variable), false);
    const auto data = environment(L"ProgramData");
    if (!data.empty()) add("start menu", fs::path(data) / L"Microsoft\\Windows\\Start Menu\\Programs", true);
    const auto public_folder = environment(L"PUBLIC");
    if (!public_folder.empty()) add("desktop", fs::path(public_folder) / L"Desktop", false);
    const auto drive = environment(L"SystemDrive");
    const fs::path users = drive.empty() ? fs::path(L"C:\\Users") : fs::path(drive + L"\\Users");
    std::error_code ec;
    for (fs::directory_iterator it(users, fs::directory_options::skip_permission_denied, ec), end;
         it != end && !ec; it.increment(ec)) {
        std::error_code type_error;
        if (!it->is_directory(type_error)) continue;
        add("start menu", it->path() / L"AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs", true);
        add("desktop", it->path() / L"Desktop", false);
        add("user programs", it->path() / L"AppData\\Local\\Programs", false);
    }
    if (const auto program_files = environment(L"ProgramFiles"); !program_files.empty())
        add("packaged apps", fs::path(program_files) / L"WindowsApps", false, FILE_NOTIFY_CHANGE_DIR_NAME);

    for (const auto& volume : system::enumerate_volumes(false)) {
        if (!volume.ready || !volume.included) continue;
        const auto root = fs::path(system::path_from_utf8(volume.root));
        add("volume", root, false, FILE_NOTIFY_CHANGE_DIR_NAME);
        for (const auto* name : {L"Program Files", L"Program Files (x86)", L"Games", L"SteamLibrary"})
            add("program files", root / name, false, FILE_NOTIFY_CHANGE_DIR_NAME);
        add("steam", root / L"SteamLibrary" / L"steamapps", false, FILE_NOTIFY_CHANGE_FILE_NAME);
    }
    add("steam", fs::path(environment(L"ProgramFiles(x86)")) / L"Steam" / L"steamapps", false,
        FILE_NOTIFY_CHANGE_FILE_NAME);
    if (extra) {
        for (const auto& folder : extra())
            add("registered folder", folder, false, FILE_NOTIFY_CHANGE_DIR_NAME);
    }
    return {found.begin(), found.end()};
}

class Watches {
public:
    Watches() = default;
    Watches(const Watches&) = delete;
    Watches& operator=(const Watches&) = delete;
    ~Watches() { clear(); }

    void clear() {
        for (auto& item : items_) {
            if (item.registry) {
                if (item.key) RegCloseKey(item.key);
                if (item.handle != INVALID_HANDLE_VALUE) CloseHandle(item.handle);
            } else if (item.handle != INVALID_HANDLE_VALUE) {
                FindCloseChangeNotification(item.handle);
            }
        }
        items_.clear();
    }

    void add_folder(const Place& place) {
        if (items_.size() >= kMaxHandles) return;
        HANDLE handle = FindFirstChangeNotificationW(place.folder.c_str(), place.subtree, place.filter);
        if (handle != INVALID_HANDLE_VALUE) items_.push_back({place.label, handle, nullptr, false});
    }

    void add_registry(HKEY root, const std::wstring& path, REGSAM view, const std::string& label) {
        if (items_.size() >= kMaxHandles) return;
        HKEY key = nullptr;
        if (RegOpenKeyExW(root, path.c_str(), 0, KEY_NOTIFY | view, &key) != ERROR_SUCCESS) return;
        HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (event == nullptr ||
            RegNotifyChangeKeyValue(key, TRUE, REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET, event,
                                    TRUE) != ERROR_SUCCESS) {
            if (event) CloseHandle(event);
            RegCloseKey(key);
            return;
        }
        items_.push_back({label, event, key, true});
    }

    [[nodiscard]] std::size_t size() const { return items_.size(); }

    [[nodiscard]] std::vector<HANDLE> handles() const {
        std::vector<HANDLE> out;
        out.reserve(items_.size());
        for (const auto& item : items_) out.push_back(item.handle);
        return out;
    }

    const Registered& arm(std::size_t index) {
        auto& item = items_[index];
        if (item.registry) {
            ResetEvent(item.handle);
            RegNotifyChangeKeyValue(item.key, TRUE, REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET,
                                    item.handle, TRUE);
        } else {
            FindNextChangeNotification(item.handle);
        }
        return item;
    }

private:
    std::vector<Registered> items_;
};

std::vector<std::wstring> loaded_user_hives() {
    std::vector<std::wstring> hives;
    HKEY users = nullptr;
    if (RegOpenKeyExW(HKEY_USERS, nullptr, 0, KEY_READ, &users) != ERROR_SUCCESS) return hives;
    wchar_t name[256];
    for (DWORD index = 0;; ++index) {
        DWORD size = 256;
        if (RegEnumKeyExW(users, index, name, &size, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;
        const std::wstring text(name, size);
        if (text.starts_with(L"S-1-5-21-") && !text.ends_with(L"_Classes")) hives.push_back(text);
    }
    RegCloseKey(users);
    return hives;
}

std::string signature_of(const std::vector<Place>& list, const std::vector<std::wstring>& hives) {
    std::string text;
    for (const auto& place : list) text += system::path_to_utf8(place.folder) + (place.subtree ? "/s;" : ";");
    for (const auto& hive : hives) text += system::to_utf8(hive) + ";";
    return text;
}

}  // namespace

ChangeWatcher::ChangeWatcher(Roots extra_roots, Callback on_change)
    : extra_roots_(std::move(extra_roots)), on_change_(std::move(on_change)) {}

ChangeWatcher::~ChangeWatcher() {
    stop();
}

void ChangeWatcher::start() {
    if (thread_.joinable()) return;
    thread_ = std::jthread([this](const std::stop_token& stop) { run(stop); });
}

void ChangeWatcher::stop() {
    if (!thread_.joinable()) return;
    thread_.request_stop();
    thread_.join();
}

void ChangeWatcher::run(const std::stop_token& stop) {
    using Clock = std::chrono::steady_clock;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    Watches watches;
    std::string active;
    auto next_recompute = Clock::now();
    std::optional<Clock::time_point> first_signal, last_signal;
    std::string reason;

    while (!stop.stop_requested()) {
        if (recompute_.exchange(false) || Clock::now() >= next_recompute) {
            next_recompute = Clock::now() + kRecompute;
            const auto list = places(extra_roots_);
            const auto hives = loaded_user_hives();
            const auto signature = signature_of(list, hives);
            if (signature != active) {
                const bool changed_set = !active.empty();
                watches.clear();
                active = signature;
                const std::wstring uninstall = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
                watches.add_registry(HKEY_LOCAL_MACHINE, uninstall, KEY_WOW64_64KEY, "registry");
                watches.add_registry(HKEY_LOCAL_MACHINE, uninstall, KEY_WOW64_32KEY, "registry");
                for (const auto& hive : hives)
                    watches.add_registry(HKEY_USERS, (hive + L"\\").append(uninstall), 0, "registry");
                for (const auto& place : list) watches.add_folder(place);
                watching_ = watches.size();
                if (changed_set && !first_signal) {
                    first_signal = last_signal = Clock::now();
                    reason = "volumes";
                }
            }
        }

        auto handles = watches.handles();
        DWORD timeout = 1000;
        if (first_signal) {
            const auto now = Clock::now();
            const auto settle = *last_signal + kQuiet;
            const auto limit = *first_signal + kLongest;
            const auto due = std::min(settle, limit);
            timeout = due <= now
                          ? 0
                          : static_cast<DWORD>(
                                std::chrono::duration_cast<std::chrono::milliseconds>(due - now).count());
        }
        const DWORD waited = handles.empty()
                                 ? (Sleep(std::min<DWORD>(timeout, 1000)), WAIT_TIMEOUT)
                                 : WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(),
                                                          FALSE, std::min<DWORD>(timeout, 1000));
        if (waited < WAIT_OBJECT_0 + handles.size()) {
            const auto& item = watches.arm(waited - WAIT_OBJECT_0);
            ++signals_;
            const auto now = Clock::now();
            if (!first_signal) {
                first_signal = now;
                reason = item.label;
            }
            last_signal = now;
        }
        if (first_signal) {
            const auto now = Clock::now();
            if (now >= *last_signal + kQuiet || now >= *first_signal + kLongest) {
                first_signal.reset();
                last_signal.reset();
                ++reports_;
                if (on_change_) on_change_(reason);
            }
        }
    }
}

}  // namespace houseguest::service
