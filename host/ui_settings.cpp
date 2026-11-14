#include "ui_settings.hpp"

#include <windows.h>

#include <filesystem>
#include <fstream>

#include "houseguest/core/serialization.hpp"

namespace houseguest::host {
namespace {

using core::Json;

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"Houseguest";

std::filesystem::path settings_path() {
    wchar_t buffer[MAX_PATH] = {};
    GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
    return std::filesystem::path(buffer) / L"Houseguest" / L"ui.json";
}

bool known_theme(const std::string& theme) {
    return theme == "system" || theme == "light" || theme == "dark";
}

}  // namespace

UiSettings load_ui_settings() {
    UiSettings settings;
    settings.start_with_windows = autostart_enabled();
    std::ifstream in(settings_path(), std::ios::binary);
    if (!in) return settings;
    const auto json = Json::parse(in, nullptr, false);
    if (!json.is_object()) return settings;
    settings.minimize_to_tray = json.value("minimize_to_tray", true);
    const auto theme = json.value("theme", std::string("system"));
    if (known_theme(theme)) settings.theme = theme;
    const auto language = json.value("language", std::string());
    if (language == "en" || language == "pt-BR") settings.language = language;
    if (const auto it = json.find("placement"); it != json.end() && it->is_object()) {
        SavedPlacement placement;
        placement.x = it->value("x", 0);
        placement.y = it->value("y", 0);
        placement.width = it->value("width", 0);
        placement.height = it->value("height", 0);
        placement.maximized = it->value("maximized", false);
        if (placement.width > 0 && placement.height > 0) settings.placement = placement;
    }
    return settings;
}

void save_ui_settings(const UiSettings& settings) {
    Json json{{"minimize_to_tray", settings.minimize_to_tray},
              {"theme", settings.theme},
              {"language", settings.language}};
    if (settings.placement) {
        const auto& p = *settings.placement;
        json["placement"] = {
            {"x", p.x}, {"y", p.y}, {"width", p.width}, {"height", p.height}, {"maximized", p.maximized}};
    }
    const auto path = settings_path();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    auto temporary = path;
    temporary += L".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << json.dump(2);
        if (!out) return;
    }
    std::filesystem::rename(temporary, path, ec);
}

bool autostart_enabled() {
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) ==
           ERROR_SUCCESS;
}

bool set_autostart(bool enabled) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return false;
    LSTATUS status = ERROR_SUCCESS;
    if (enabled) {
        wchar_t module[MAX_PATH];
        const DWORD length = GetModuleFileNameW(nullptr, module, MAX_PATH);
        const std::wstring command = L"\"" + std::wstring(module, length) + L"\" --tray";
        status = RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                                static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        status = RegDeleteValueW(key, kRunValue);
        if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

bool prefers_portuguese(const UiSettings& settings) {
    if (!settings.language.empty()) return settings.language == "pt-BR";
    wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
    GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH);
    return std::wstring(name).starts_with(L"pt");
}

}  // namespace houseguest::host
