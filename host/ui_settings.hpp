#pragma once

#include <optional>
#include <string>

namespace houseguest::host {

struct SavedPlacement {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool maximized = false;
};

struct UiSettings {
    bool minimize_to_tray = true;
    bool start_with_windows = false;
    std::string theme = "system";
    std::string language;
    std::optional<SavedPlacement> placement;
};

UiSettings load_ui_settings();
void save_ui_settings(const UiSettings& settings);
bool set_autostart(bool enabled);
bool autostart_enabled();
bool prefers_portuguese(const UiSettings& settings);

}  // namespace houseguest::host
