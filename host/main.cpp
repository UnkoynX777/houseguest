#include <windows.h>

#include <dwmapi.h>
#include <shellapi.h>
#include <wrl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "WebView2.h"
#ifdef HOUSEGUEST_DEVTOOLS
#include "WebView2EnvironmentOptions.h"
#endif
#include "houseguest/system/pipe.hpp"
#include "houseguest/system/text.hpp"

#include "dialogs.hpp"
#include "geometry.hpp"
#include "icons.hpp"
#include "recovery.hpp"
#include "service_monitor.hpp"
#include "tray.hpp"
#include "ui_settings.hpp"

using houseguest::core::Json;
using houseguest::system::to_utf8;
using houseguest::system::to_wide;
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
namespace host = houseguest::host;

namespace {

constexpr wchar_t kWindowClass[] = L"HouseguestWindow";
constexpr wchar_t kHost[] = L"app.houseguest";
constexpr wchar_t kOrigin[] = L"https://app.houseguest/";
constexpr int kMaxInFlightRequests = 8;
constexpr UINT kReplyMessage = WM_APP + 1;
constexpr UINT kTrayMessage = WM_APP + 2;
constexpr UINT kStatusMessage = WM_APP + 3;
constexpr UINT kShowMessage = WM_APP + 4;
constexpr UINT kRecoveryMessage = WM_APP + 5;
constexpr UINT_PTR kTooltipTimer = 1;
constexpr UINT kTooltipInterval = 15000;
constexpr COREWEBVIEW2_COLOR kDarkBackground{255, 27, 27, 29};
constexpr COREWEBVIEW2_COLOR kLightBackground{255, 243, 243, 241};

struct App {
    HWND window = nullptr;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> view;
    host::UiSettings settings;
    std::unique_ptr<host::Tray> tray;
    std::unique_ptr<host::ServiceMonitor> monitor;
    std::unique_ptr<host::IconService> icons;
    host::ServiceStatus status;
    std::chrono::steady_clock::time_point status_time;
    bool have_status = false;
    bool exiting = false;
    bool hint_shown = false;
    UINT taskbar_created = 0;
};

App g;

bool system_uses_dark_theme() {
    DWORD value = 1, size = sizeof(value);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return value == 0;
}

bool dark_in_effect() {
    return g.settings.theme == "dark" || (g.settings.theme == "system" && system_uses_dark_theme());
}

bool portuguese() {
    return host::prefers_portuguese(g.settings);
}

void apply_theme() {
    const BOOL dark = dark_in_effect();
    if (g.window) {
        DwmSetWindowAttribute(g.window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        const COLORREF caption = dark ? RGB(27, 27, 29) : RGB(243, 243, 241);
        DwmSetWindowAttribute(g.window, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
        static HBRUSH dark_brush = CreateSolidBrush(RGB(27, 27, 29));
        static HBRUSH light_brush = CreateSolidBrush(RGB(243, 243, 241));
        SetClassLongPtrW(g.window, GCLP_HBRBACKGROUND,
                         reinterpret_cast<LONG_PTR>(dark ? dark_brush : light_brush));
        RedrawWindow(g.window, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_ERASE);
    }
    if (!g.controller) return;
    ComPtr<ICoreWebView2Controller2> controller2;
    if (SUCCEEDED(g.controller.As(&controller2))) {
        controller2->put_DefaultBackgroundColor(dark ? kDarkBackground : kLightBackground);
    }
    ComPtr<ICoreWebView2_13> view13;
    ComPtr<ICoreWebView2Profile> profile;
    if (g.view && SUCCEEDED(g.view.As(&view13)) && SUCCEEDED(view13->get_Profile(&profile))) {
        profile->put_PreferredColorScheme(
            g.settings.theme == "system" ? COREWEBVIEW2_PREFERRED_COLOR_SCHEME_AUTO
            : g.settings.theme == "dark" ? COREWEBVIEW2_PREFERRED_COLOR_SCHEME_DARK
                                         : COREWEBVIEW2_PREFERRED_COLOR_SCHEME_LIGHT);
    }
}

std::filesystem::path executable_directory() {
    std::wstring buffer(MAX_PATH, L'\0');
    buffer.resize(GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size())));
    return std::filesystem::path(buffer).parent_path();
}

std::filesystem::path user_data_directory() {
    wchar_t buffer[MAX_PATH] = {};
    GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
    return std::filesystem::path(buffer) / L"Houseguest" / L"WebView2";
}

Json failure(const Json& id, const char* code, const char* message) {
    return {{"id", id},
            {"ok", false},
            {"error", {{"code", code}, {"message", message}, {"details", Json::object()}}}};
}

void post_to_view(const Json& message) {
    auto* text = new std::wstring(to_wide(message.dump()));
    if (!PostMessageW(g.window, kReplyMessage, 0, reinterpret_cast<LPARAM>(text))) delete text;
}

void show_window() {
    if (!IsWindowVisible(g.window)) ShowWindow(g.window, SW_SHOW);
    if (IsIconic(g.window)) ShowWindow(g.window, SW_RESTORE);
    SetForegroundWindow(g.window);
}

void navigate_to(const char* route) {
    show_window();
    post_to_view({{"event", "navigate"}, {"route", route}});
}

void save_settings() {
    if (const auto placement = host::capture_placement(g.window)) g.settings.placement = placement;
    host::save_ui_settings(g.settings);
}

Json prefs_json() {
    return {{"minimize_to_tray", g.settings.minimize_to_tray},
            {"start_with_windows", g.settings.start_with_windows},
            {"theme", g.settings.theme}};
}

Json update_prefs(const Json& args) {
    if (args.contains("minimize_to_tray") && args["minimize_to_tray"].is_boolean()) {
        g.settings.minimize_to_tray = args["minimize_to_tray"].get<bool>();
    }
    if (args.contains("start_with_windows") && args["start_with_windows"].is_boolean()) {
        const bool wanted = args["start_with_windows"].get<bool>();
        if (host::set_autostart(wanted)) g.settings.start_with_windows = wanted;
    }
    if (args.contains("theme") && args["theme"].is_string()) {
        const auto theme = args["theme"].get<std::string>();
        if (theme == "system" || theme == "light" || theme == "dark") g.settings.theme = theme;
        apply_theme();
    }
    if (args.contains("language") && args["language"].is_string()) {
        const auto language = args["language"].get<std::string>();
        if (language == "en" || language == "pt-BR") g.settings.language = language;
    }
    save_settings();
    return prefs_json();
}

void restart_interface() {
    wchar_t module[MAX_PATH];
    GetModuleFileNameW(nullptr, module, MAX_PATH);
    const auto arguments = L"--after " + std::to_wstring(GetCurrentProcessId());
    ShellExecuteW(nullptr, L"open", module, arguments.c_str(), nullptr, SW_SHOWNORMAL);
    g.exiting = true;
    DestroyWindow(g.window);
}

void open_recovery() {
    static bool open = false;
    if (open) return;
    open = true;
    const auto outcome = host::show_recovery(g.window, portuguese());
    open = false;
    if (outcome == host::RecoveryResult::RestartInterface) restart_interface();
}

void handle_host_command(const Json& id, const std::string& command, const Json& args) {
    Json data;
    if (command == "host.icon") {
        const auto path = args.value("path", std::string());
        const int pixels = std::clamp(args.value("size", 48), 16, 128);
        if (!g.icons || path.empty() || path.size() > 1024) {
            post_to_view({{"id", id}, {"ok", true}, {"data", {{"png", ""}}}});
            return;
        }
        g.icons->request(path, pixels, [id](std::string url) {
            post_to_view({{"id", id}, {"ok", true}, {"data", {{"png", std::move(url)}}}});
        });
        return;
    }
    if (command == "host.recovery") {
        PostMessageW(g.window, kRecoveryMessage, 0, 0);
        data = {{"opened", true}};
    } else if (command == "host.elevate") {
        const bool started = host::relaunch_elevated();
        if (started) {
            g.exiting = true;
            PostMessageW(g.window, WM_CLOSE, 0, 0);
        }
        data = {{"started", started}};
    } else if (command == "host.save_text") {
        data = host::save_text(g.window, args);
    } else if (command == "host.pick_paths") {
        data = host::pick_paths(g.window, args);
    } else if (command == "host.open_text") {
        data = host::open_text(g.window);
    } else if (command == "host.info") {
        data = {{"elevated", host::is_elevated()}};
    } else if (command == "host.get_prefs") {
        data = prefs_json();
    } else if (command == "host.set_prefs") {
        data = update_prefs(args);
    } else {
        post_to_view(failure(id, "bad_request", "Unknown host command."));
        return;
    }
    post_to_view({{"id", id}, {"ok", true}, {"data", std::move(data)}});
}

void handle_message(const std::string& text) {
    auto request = Json::parse(text, nullptr, false);
    if (request.is_discarded() || !request.is_object() || !request.contains("cmd") ||
        !request["cmd"].is_string()) {
        return;
    }
    const Json id = request.value("id", Json(nullptr));
    const auto command = request["cmd"].get<std::string>();
    if (command.starts_with("host.")) {
        handle_host_command(id, command, request.value("args", Json::object()));
        return;
    }
    static std::atomic<int> in_flight = 0;
    if (in_flight.fetch_add(1) >= kMaxInFlightRequests) {
        in_flight.fetch_sub(1);
        post_to_view(failure(id, "busy", "Too many requests are waiting for the service."));
        return;
    }
    try {
        std::thread([request = std::move(request), id] {
            houseguest::system::PipeClient client;
            auto reply = client.call(request);
            post_to_view(reply
                             ? *reply
                             : failure(id, "service_unavailable", "The Houseguest service is not running."));
            in_flight.fetch_sub(1);
        }).detach();
    } catch (const std::system_error&) {
        in_flight.fetch_sub(1);
        post_to_view(failure(id, "busy", "The request could not be started."));
    }
}

bool user_is_looking() {
    return IsWindowVisible(g.window) && !IsIconic(g.window) && GetForegroundWindow() == g.window;
}

std::int64_t remaining_seconds() {
    if (!g.status.armed) return 0;
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - g.status_time);
    return std::max<std::int64_t>(0, g.status.ends_at - g.status.server_now - elapsed.count());
}

void refresh_tooltip() {
    g.tray->set_tooltip(host::describe_status(g.status, remaining_seconds(), portuguese()).tooltip);
}

void on_status(std::unique_ptr<host::ServiceStatus> next) {
    const bool had = g.have_status;
    const auto previous = g.status;
    g.status = *next;
    g.status_time = std::chrono::steady_clock::now();
    g.have_status = true;
    refresh_tooltip();
    if (!had || user_is_looking()) return;

    const bool pt = portuguese();
    if (previous.reachable && !next->reachable) {
        g.tray->balloon(L"Houseguest", host::service_lost_message(pt), true);
    } else if (!previous.reachable && next->reachable) {
        g.tray->balloon(L"Houseguest", host::service_back_message(pt), false);
    } else if (previous.armed && !next->armed && next->reachable) {
        g.tray->balloon(L"Houseguest", host::session_ended_message(pt), false);
    } else if (next->armed && previous.enforcement_available && !next->enforcement_available) {
        g.tray->balloon(L"Houseguest", host::enforcement_lost_message(pt), true);
    }
}

void on_tray_menu() {
    const auto text = host::describe_status(g.status, remaining_seconds(), portuguese());
    const host::TrayMenuState state{g.settings.minimize_to_tray, g.settings.start_with_windows, portuguese()};
    const auto command = g.tray->show_menu(text, state);
    if (!command) return;
    switch (*command) {
        case host::TrayCommand::Open:
            show_window();
            break;
        case host::TrayCommand::Settings:
            navigate_to("settings");
            break;
        case host::TrayCommand::Recovery:
            open_recovery();
            break;
        case host::TrayCommand::ToggleMinimizeToTray:
            update_prefs({{"minimize_to_tray", !g.settings.minimize_to_tray}});
            post_to_view({{"event", "prefs"}, {"prefs", prefs_json()}});
            break;
        case host::TrayCommand::ToggleAutostart:
            update_prefs({{"start_with_windows", !g.settings.start_with_windows}});
            post_to_view({{"event", "prefs"}, {"prefs", prefs_json()}});
            break;
        case host::TrayCommand::Exit:
            g.exiting = true;
            DestroyWindow(g.window);
            break;
    }
}

void open_external(const std::wstring& target) {
    if (target.starts_with(L"https://"))
        ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

std::wstring uri_of(const std::function<HRESULT(PWSTR*)>& getter) {
    PWSTR uri = nullptr;
    getter(&uri);
    std::wstring target = uri ? uri : L"";
    CoTaskMemFree(uri);
    return target;
}

void configure_settings() {
    ComPtr<ICoreWebView2Settings> settings;
    g.view->get_Settings(&settings);
    settings->put_AreDefaultContextMenusEnabled(FALSE);
    settings->put_IsStatusBarEnabled(FALSE);
    settings->put_IsZoomControlEnabled(FALSE);
    settings->put_AreHostObjectsAllowed(FALSE);
    settings->put_AreDefaultScriptDialogsEnabled(FALSE);
    ComPtr<ICoreWebView2Settings3> settings3;
    if (SUCCEEDED(settings.As(&settings3))) settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
    ComPtr<ICoreWebView2Settings4> settings4;
    if (SUCCEEDED(settings.As(&settings4))) {
        settings4->put_IsPasswordAutosaveEnabled(FALSE);
        settings4->put_IsGeneralAutofillEnabled(FALSE);
    }
#ifndef HOUSEGUEST_DEVTOOLS
    settings->put_AreDevToolsEnabled(FALSE);
#endif
}

void register_handlers() {
    EventRegistrationToken token;
    g.view->add_ProcessFailed(Callback<ICoreWebView2ProcessFailedEventHandler>(
                                  [](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT {
                                      COREWEBVIEW2_PROCESS_FAILED_KIND kind{};
                                      args->get_ProcessFailedKind(&kind);
                                      if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED)
                                          PostMessageW(g.window, kRecoveryMessage, 0, 0);
                                      return S_OK;
                                  })
                                  .Get(),
                              &token);
    g.view->add_NavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                const auto target = uri_of([&](PWSTR* uri) { return args->get_Uri(uri); });
                if (!target.starts_with(kOrigin)) {
                    args->put_Cancel(TRUE);
                    open_external(target);
                }
                return S_OK;
            })
            .Get(),
        &token);
    g.view->add_PermissionRequested(
        Callback<ICoreWebView2PermissionRequestedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT {
                args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);
                return S_OK;
            })
            .Get(),
        &token);
    ComPtr<ICoreWebView2_4> view4;
    if (SUCCEEDED(g.view.As(&view4))) {
        view4->add_FrameCreated(
            Callback<ICoreWebView2FrameCreatedEventHandler>([](ICoreWebView2*,
                                                               ICoreWebView2FrameCreatedEventArgs* args)
                                                                -> HRESULT {
                ComPtr<ICoreWebView2Frame> frame;
                if (SUCCEEDED(args->get_Frame(&frame))) {
                    ComPtr<ICoreWebView2Frame2> frame2;
                    EventRegistrationToken frame_token;
                    if (SUCCEEDED(frame.As(&frame2))) {
                        frame2->add_NavigationStarting(
                            Callback<ICoreWebView2FrameNavigationStartingEventHandler>(
                                [](ICoreWebView2Frame*,
                                   ICoreWebView2NavigationStartingEventArgs* nav) -> HRESULT {
                                    nav->put_Cancel(TRUE);
                                    return S_OK;
                                })
                                .Get(),
                            &frame_token);
                    }
                }
                return S_OK;
            }).Get(),
            &token);
    }
    g.view->add_NewWindowRequested(
        Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                args->put_Handled(TRUE);
                open_external(uri_of([&](PWSTR* uri) { return args->get_Uri(uri); }));
                return S_OK;
            })
            .Get(),
        &token);
    g.view->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                const auto source = uri_of([&](PWSTR* uri) { return args->get_Source(uri); });
                PWSTR message = nullptr;
                if (source.starts_with(kOrigin) && SUCCEEDED(args->TryGetWebMessageAsString(&message))) {
                    handle_message(to_utf8(message));
                    CoTaskMemFree(message);
                }
                return S_OK;
            })
            .Get(),
        &token);
}

HRESULT on_controller_ready(HRESULT result, ICoreWebView2Controller* controller) {
    if (FAILED(result) || !controller) {
        PostMessageW(g.window, kRecoveryMessage, 0, 0);
        return result;
    }
    g.controller = controller;
    g.controller->get_CoreWebView2(&g.view);
    configure_settings();
    apply_theme();

    ComPtr<ICoreWebView2_3> view3;
    if (SUCCEEDED(g.view.As(&view3))) {
        const auto ui_directory = (executable_directory() / L"ui").wstring();
        view3->SetVirtualHostNameToFolderMapping(kHost, ui_directory.c_str(),
                                                 COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
    }
    register_handlers();

    RECT bounds;
    GetClientRect(g.window, &bounds);
    g.controller->put_Bounds(bounds);
    g.view->Navigate((std::wstring(kOrigin) + L"index.html").c_str());
    return S_OK;
}

void resize_view() {
    if (!g.controller) return;
    RECT bounds;
    GetClientRect(g.window, &bounds);
    g.controller->put_Bounds(bounds);
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (g.taskbar_created != 0 && message == g.taskbar_created) {
        g.tray->add();
        refresh_tooltip();
        return 0;
    }
    switch (message) {
        case WM_SIZE:
            if (wparam == SIZE_MINIMIZED && g.settings.minimize_to_tray && !g.exiting) {
                ShowWindow(window, SW_HIDE);
                return 0;
            }
            resize_view();
            return 0;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            if (wparam != FALSE) {
                g.exiting = true;
                DestroyWindow(window);
            }
            return 0;
        case WM_EXITSIZEMOVE:
            save_settings();
            return 0;
        case WM_GETMINMAXINFO: {
            const SIZE minimum = host::minimum_size(window);
            reinterpret_cast<MINMAXINFO*>(lparam)->ptMinTrackSize = {minimum.cx, minimum.cy};
            return 0;
        }
        case WM_DPICHANGED: {
            const auto* suggested = reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_SETTINGCHANGE:
            if (lparam != 0 &&
                std::wstring_view(reinterpret_cast<const wchar_t*>(lparam)) == L"ImmersiveColorSet") {
                apply_theme();
            }
            return 0;
        case WM_TIMER:
            if (wparam == kTooltipTimer) refresh_tooltip();
            return 0;
        case WM_CLOSE:
            if (g.settings.minimize_to_tray && !g.exiting) {
                save_settings();
                ShowWindow(window, SW_HIDE);
                if (!g.hint_shown) {
                    g.hint_shown = true;
                    g.tray->balloon(L"Houseguest", host::tray_hint_message(portuguese()), false);
                }
                return 0;
            }
            g.exiting = true;
            DestroyWindow(window);
            return 0;
        case kTrayMessage:
            switch (LOWORD(lparam)) {
                case NIN_SELECT:
                case NIN_KEYSELECT:
                    show_window();
                    break;
                case WM_CONTEXTMENU:
                    on_tray_menu();
                    break;
                default:
                    break;
            }
            return 0;
        case kStatusMessage:
            on_status(std::unique_ptr<host::ServiceStatus>(reinterpret_cast<host::ServiceStatus*>(lparam)));
            return 0;
        case kShowMessage:
            show_window();
            return 0;
        case kRecoveryMessage:
            show_window();
            open_recovery();
            return 0;
        case kReplyMessage: {
            std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lparam));
            if (g.view) g.view->PostWebMessageAsString(text->c_str());
            return 0;
        }
        case WM_DESTROY:
            KillTimer(window, kTooltipTimer);
            save_settings();
            g.monitor.reset();
            g.icons.reset();
            g.tray.reset();
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window, message, wparam, lparam);
    }
}

HWND create_main_window(HINSTANCE instance, bool& maximized) {
    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    window_class.hIconSm = window_class.hIcon;
    window_class.hbrBackground = CreateSolidBrush(dark_in_effect() ? RGB(27, 27, 29) : RGB(243, 243, 241));
    window_class.lpszClassName = kWindowClass;
    RegisterClassExW(&window_class);

    const auto initial = host::initial_window(g.settings.placement);
    const RECT& r = initial.bounds;
    HWND window = CreateWindowExW(0, kWindowClass, L"Houseguest", WS_OVERLAPPEDWINDOW, r.left, r.top,
                                  r.right - r.left, r.bottom - r.top, nullptr, nullptr, instance, nullptr);
    maximized = initial.maximized;
    return window;
}

struct Launch {
    bool hidden = false;
    bool recovery = false;
    DWORD wait_for = 0;
};

Launch parse_launch() {
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    Launch launch;
    for (int i = 1; i < count; ++i) {
        const std::wstring_view argument = arguments[i];
        launch.hidden = launch.hidden || argument == L"--tray";
        launch.recovery = launch.recovery || argument == L"--recovery";
        if (argument == L"--after" && i + 1 < count)
            launch.wait_for = static_cast<DWORD>(_wtoi(arguments[i + 1]));
    }
    LocalFree(reinterpret_cast<HLOCAL>(arguments));
    return launch;
}

void wait_for_previous_instance(DWORD pid) {
    if (pid == 0) return;
    if (HANDLE previous = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
        WaitForSingleObject(previous, 10000);
        CloseHandle(previous);
    }
}

std::filesystem::path icon_cache_directory() {
    return user_data_directory().parent_path() / L"icons";
}

void create_webview(const std::filesystem::path& data_directory) {
    ICoreWebView2EnvironmentOptions* options = nullptr;
#ifdef HOUSEGUEST_DEVTOOLS
    auto debug_options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    debug_options->put_AdditionalBrowserArguments(L"--remote-debugging-port=9333");
    options = debug_options.Get();
#endif
    CreateCoreWebView2EnvironmentWithOptions(
        nullptr, data_directory.c_str(), options,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
                if (FAILED(result) || !environment) {
                    host::offer_webview_runtime(g.window, portuguese());
                    PostMessageW(g.window, kRecoveryMessage, 0, 0);
                    return result;
                }
                return environment->CreateCoreWebView2Controller(
                    g.window, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                                  [](HRESULT r, ICoreWebView2Controller* controller) -> HRESULT {
                                      return on_controller_ready(r, controller);
                                  })
                                  .Get());
            })
            .Get());
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    const auto launch = parse_launch();
    wait_for_previous_instance(launch.wait_for);
    HANDLE single = CreateMutexW(
        nullptr, TRUE, host::is_elevated() ? L"Local\\HouseguestUI.elevated" : L"Local\\HouseguestUI");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(kWindowClass, nullptr)) {
            PostMessageW(existing, kShowMessage, 0, 0);
            SetForegroundWindow(existing);
        }
        return 0;
    }
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;

    g.settings = host::load_ui_settings();
    bool maximized = false;
    g.window = create_main_window(instance, maximized);
    g.taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    g.tray = std::make_unique<host::Tray>(g.window, kTrayMessage);
    g.tray->add();
    apply_theme();

    if (!host::webview_runtime_installed()) host::offer_webview_runtime(g.window, portuguese());

    const bool hidden = launch.hidden;
    if (!hidden) ShowWindow(g.window, maximized ? SW_SHOWMAXIMIZED : SW_SHOW);

    const auto data_directory = user_data_directory();
    std::filesystem::create_directories(data_directory);
    create_webview(data_directory);
    g.icons = std::make_unique<host::IconService>(icon_cache_directory());
    g.monitor = std::make_unique<host::ServiceMonitor>(g.window, kStatusMessage);
    if (launch.recovery) PostMessageW(g.window, kRecoveryMessage, 0, 0);
    SetTimer(g.window, kTooltipTimer, kTooltipInterval, nullptr);

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    g.view.Reset();
    g.controller.Reset();
    CoUninitialize();
    CloseHandle(single);
    return static_cast<int>(message.wParam);
}
