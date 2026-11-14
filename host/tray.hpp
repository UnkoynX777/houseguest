#pragma once

#include <windows.h>

#include <shellapi.h>

#include <optional>
#include <string>

#include "service_monitor.hpp"

namespace houseguest::host {

enum class TrayCommand { Open, Settings, ToggleMinimizeToTray, ToggleAutostart, Exit };

struct TrayText {
    std::wstring headline;
    std::wstring detail;
    std::wstring tooltip;
};

struct TrayMenuState {
    bool minimize_to_tray = true;
    bool autostart = false;
    bool portuguese = false;
};

TrayText describe_status(const ServiceStatus& status, std::int64_t remaining_seconds, bool portuguese);
std::wstring session_ended_message(bool portuguese);
std::wstring service_lost_message(bool portuguese);
std::wstring service_back_message(bool portuguese);
std::wstring enforcement_lost_message(bool portuguese);
std::wstring tray_hint_message(bool portuguese);

class Tray {
public:
    Tray(HWND owner, UINT callback_message);
    ~Tray();
    Tray(const Tray&) = delete;
    Tray& operator=(const Tray&) = delete;

    bool add();
    void remove();
    void set_tooltip(const std::wstring& text);
    void balloon(const std::wstring& title, const std::wstring& text, bool error);
    std::optional<TrayCommand> show_menu(const TrayText& text, const TrayMenuState& state);

private:
    NOTIFYICONDATAW data() const;

    HWND owner_;
    UINT callback_message_;
    HICON icon_ = nullptr;
    bool added_ = false;
};

}  // namespace houseguest::host
