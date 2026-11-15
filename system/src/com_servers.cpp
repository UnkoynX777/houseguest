#include "houseguest/system/com_servers.hpp"

#include <windows.h>

#include <array>
#include <memory>

#include "houseguest/system/text.hpp"

#include "catalog_support.hpp"

namespace houseguest::system {
namespace {

constexpr wchar_t kClsid[] = L"SOFTWARE\\Classes\\CLSID";
constexpr std::array<REGSAM, 2> kViews = {KEY_WOW64_64KEY, KEY_WOW64_32KEY};

struct KeyCloser {
    void operator()(HKEY key) const { RegCloseKey(key); }
};
using Key = std::unique_ptr<std::remove_pointer_t<HKEY>, KeyCloser>;

struct EventCloser {
    void operator()(HANDLE handle) const { CloseHandle(handle); }
};
using Event = std::unique_ptr<void, EventCloser>;

}  // namespace

struct ComServerIndex::Watch {
    struct View {
        Key key;
        Event signal;
    };
    std::array<View, 2> views;

    bool arm() {
        bool ok = false;
        for (std::size_t i = 0; i < views.size(); ++i) {
            HKEY raw = nullptr;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kClsid, 0, KEY_READ | KEY_NOTIFY | kViews[i], &raw) !=
                ERROR_SUCCESS)
                continue;
            views[i].key.reset(raw);
            views[i].signal.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            ok |= views[i].signal &&
                  RegNotifyChangeKeyValue(raw, TRUE, REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET,
                                          views[i].signal.get(), TRUE) == ERROR_SUCCESS;
        }
        return ok;
    }

    bool changed() const {
        for (const auto& view : views) {
            if (view.signal && WaitForSingleObject(view.signal.get(), 0) == WAIT_OBJECT_0) return true;
        }
        return false;
    }
};

ComServerIndex::ComServerIndex() = default;
ComServerIndex::~ComServerIndex() = default;

std::filesystem::path ComServerIndex::executable_of(std::wstring command) {
    const auto first = command.find_first_not_of(L" \t");
    if (first == std::wstring::npos) return {};
    command.erase(0, first);
    std::wstring program;
    if (command.front() == L'"') {
        const auto close = command.find(L'"', 1);
        program = command.substr(1, close == std::wstring::npos ? std::wstring::npos : close - 1);
    } else {
        std::wstring lowered = command;
        for (auto& c : lowered) c = static_cast<wchar_t>(towlower(c));
        const auto exe = lowered.find(L".exe");
        program = exe == std::wstring::npos ? command : command.substr(0, exe + 4);
    }
    return catalog::expand(std::move(program));
}

void ComServerIndex::refresh_locked() {
    if (built_ && watch_ && !watch_->changed()) return;
    auto next = std::make_unique<Watch>();
    next->arm();
    std::unordered_set<std::string> found;
    for (const REGSAM view : kViews) {
        for (const auto& clsid : catalog::subkeys(HKEY_LOCAL_MACHINE, kClsid, view)) {
            const auto command = catalog::registry_text(
                HKEY_LOCAL_MACHINE, std::wstring(kClsid) + L"\\" + clsid + L"\\LocalServer32", nullptr, view);
            if (command.empty()) continue;
            const auto program = executable_of(command);
            if (!program.empty()) found.insert(catalog::canonical_key(program));
        }
    }
    servers_ = std::move(found);
    watch_ = std::move(next);
    built_ = true;
    rebuilds_.fetch_add(1);
}

bool ComServerIndex::registered(const std::filesystem::path& image) {
    const auto key = catalog::canonical_key(image);
    std::scoped_lock lock(mutex_);
    refresh_locked();
    return servers_.contains(key);
}

std::size_t ComServerIndex::size() {
    std::scoped_lock lock(mutex_);
    refresh_locked();
    return servers_.size();
}

std::size_t ComServerIndex::rebuilds() const {
    return rebuilds_.load();
}

}  // namespace houseguest::system
