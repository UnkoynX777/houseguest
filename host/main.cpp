#include <windows.h>

#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wrl.h>

#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "WebView2.h"
#ifdef HOUSEGUEST_DEVTOOLS
#include "WebView2EnvironmentOptions.h"
#endif
#include "houseguest/system/pipe.hpp"
#include "houseguest/system/text.hpp"

using houseguest::core::Json;
using houseguest::system::to_utf8;
using houseguest::system::to_wide;
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kWindowClass[] = L"HouseguestWindow";
constexpr wchar_t kHost[] = L"app.houseguest";
constexpr wchar_t kOrigin[] = L"https://app.houseguest/";
constexpr UINT kReplyMessage = WM_APP + 1;
constexpr std::size_t kMaxFileBytes = 2 * 1024 * 1024;

HWND g_window = nullptr;
ComPtr<ICoreWebView2Controller> g_controller;
ComPtr<ICoreWebView2> g_view;

bool system_uses_dark_theme() {
    DWORD value = 1, size = sizeof(value);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return value == 0;
}

void apply_title_bar_theme(HWND window) {
    const BOOL dark = system_uses_dark_theme();
    DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
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

void post_reply(const Json& reply) {
    auto* text = new std::wstring(to_wide(reply.dump()));
    if (!PostMessageW(g_window, kReplyMessage, 0, reinterpret_cast<LPARAM>(text))) delete text;
}

Json relaunch_elevated() {
    wchar_t module[MAX_PATH];
    GetModuleFileNameW(nullptr, module, MAX_PATH);
    SHELLEXECUTEINFOW info{sizeof(info)};
    info.lpVerb = L"runas";
    info.lpFile = module;
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) return {{"started", false}};
    PostMessageW(g_window, WM_CLOSE, 0, 0);
    return {{"started", true}};
}

Json save_text(const Json& args) {
    const auto name = to_wide(args.value("name", "export.json"));
    const auto content = args.value("content", "");
    ComPtr<IFileSaveDialog> dialog;
    if (FAILED(
            CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        return {{"saved", false}};
    }
    dialog->SetFileName(name.c_str());
    dialog->SetDefaultExtension(L"json");
    if (FAILED(dialog->Show(g_window))) return {{"saved", false}};
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dialog->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        return {{"saved", false}};
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    CoTaskMemFree(path);
    out << content;
    return {{"saved", static_cast<bool>(out)}};
}

Json open_text() {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(
            CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        return {{"opened", false}};
    }
    const COMDLG_FILTERSPEC filter[] = {{L"JSON", L"*.json"}};
    dialog->SetFileTypes(1, filter);
    if (FAILED(dialog->Show(g_window))) return {{"opened", false}};
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dialog->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        return {{"opened", false}};
    std::ifstream in(path, std::ios::binary);
    CoTaskMemFree(path);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    if (buffer.str().size() > kMaxFileBytes) return {{"opened", false}, {"reason", "file too large"}};
    return {{"opened", true}, {"content", buffer.str()}};
}

bool is_elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated;
}

void handle_message(const std::string& text) {
    auto request = Json::parse(text, nullptr, false);
    if (request.is_discarded() || !request.is_object() || !request.contains("cmd") ||
        !request["cmd"].is_string())
        return;
    const Json id = request.value("id", Json(nullptr));
    const auto command = request["cmd"].get<std::string>();

    if (command.starts_with("host.")) {
        const Json args = request.value("args", Json::object());
        Json data;
        if (command == "host.elevate")
            data = relaunch_elevated();
        else if (command == "host.save_text")
            data = save_text(args);
        else if (command == "host.open_text")
            data = open_text();
        else if (command == "host.info")
            data = {{"elevated", is_elevated()}};
        else {
            post_reply(failure(id, "bad_request", "Unknown host command."));
            return;
        }
        post_reply({{"id", id}, {"ok", true}, {"data", std::move(data)}});
        return;
    }

    std::thread([request = std::move(request), id] {
        houseguest::system::PipeClient client;
        auto reply = client.call(request);
        post_reply(reply ? *reply
                         : failure(id, "service_unavailable", "The Houseguest service is not running."));
    }).detach();
}

HRESULT on_controller_ready(HWND window, HRESULT result, ICoreWebView2Controller* controller) {
    if (FAILED(result) || !controller) {
        MessageBoxW(window, L"The WebView2 runtime could not be started.", L"Houseguest", MB_ICONERROR);
        return result;
    }
    g_controller = controller;
    g_controller->get_CoreWebView2(&g_view);

    ComPtr<ICoreWebView2Settings> settings;
    g_view->get_Settings(&settings);
    settings->put_AreDefaultContextMenusEnabled(FALSE);
    settings->put_IsStatusBarEnabled(FALSE);
    settings->put_IsZoomControlEnabled(FALSE);
    ComPtr<ICoreWebView2Settings3> settings3;
    if (SUCCEEDED(settings.As(&settings3))) settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
#ifndef HOUSEGUEST_DEVTOOLS
    settings->put_AreDevToolsEnabled(FALSE);
#endif

    ComPtr<ICoreWebView2_3> view3;
    if (SUCCEEDED(g_view.As(&view3))) {
        const auto ui_directory = (executable_directory() / L"ui").wstring();
        view3->SetVirtualHostNameToFolderMapping(kHost, ui_directory.c_str(),
                                                 COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
    }

    EventRegistrationToken token;
    g_view->add_NavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                PWSTR uri = nullptr;
                args->get_Uri(&uri);
                const std::wstring target = uri ? uri : L"";
                CoTaskMemFree(uri);
                if (!target.starts_with(kOrigin)) {
                    args->put_Cancel(TRUE);
                    if (target.starts_with(L"https://"))
                        ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
                return S_OK;
            })
            .Get(),
        &token);

    g_view->add_NewWindowRequested(
        Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                PWSTR uri = nullptr;
                args->get_Uri(&uri);
                const std::wstring target = uri ? uri : L"";
                CoTaskMemFree(uri);
                args->put_Handled(TRUE);
                if (target.starts_with(L"https://"))
                    ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                return S_OK;
            })
            .Get(),
        &token);

    g_view->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                PWSTR source = nullptr;
                args->get_Source(&source);
                const bool trusted = source && std::wstring(source).starts_with(kOrigin);
                CoTaskMemFree(source);
                PWSTR message = nullptr;
                if (trusted && SUCCEEDED(args->TryGetWebMessageAsString(&message))) {
                    handle_message(to_utf8(message));
                    CoTaskMemFree(message);
                }
                return S_OK;
            })
            .Get(),
        &token);

    RECT bounds;
    GetClientRect(window, &bounds);
    g_controller->put_Bounds(bounds);
    g_view->Navigate((std::wstring(kOrigin) + L"index.html").c_str());
    return S_OK;
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_SIZE:
            if (g_controller) {
                RECT bounds;
                GetClientRect(window, &bounds);
                g_controller->put_Bounds(bounds);
            }
            return 0;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            const UINT dpi = GetDpiForWindow(window);
            info->ptMinTrackSize = {MulDiv(900, dpi, 96), MulDiv(620, dpi, 96)};
            return 0;
        }
        case WM_SETTINGCHANGE:
            apply_title_bar_theme(window);
            return 0;
        case kReplyMessage: {
            std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lparam));
            if (g_view) g_view->PostWebMessageAsString(text->c_str());
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window, message, wparam, lparam);
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    HANDLE single =
        CreateMutexW(nullptr, TRUE, is_elevated() ? L"Local\\HouseguestUI.elevated" : L"Local\\HouseguestUI");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(kWindowClass, nullptr)) SetForegroundWindow(existing);
        return 0;
    }
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;

    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    window_class.hbrBackground =
        CreateSolidBrush(system_uses_dark_theme() ? RGB(15, 20, 32) : RGB(250, 248, 244));
    window_class.lpszClassName = kWindowClass;
    RegisterClassExW(&window_class);

    const UINT dpi = GetDpiForSystem();
    g_window =
        CreateWindowExW(0, kWindowClass, L"Houseguest", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                        MulDiv(1180, dpi, 96), MulDiv(760, dpi, 96), nullptr, nullptr, instance, nullptr);
    apply_title_bar_theme(g_window);
    ShowWindow(g_window, show);

    const auto data_directory = user_data_directory();
    std::filesystem::create_directories(data_directory);
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
                    MessageBoxW(g_window, L"The WebView2 runtime is not installed.", L"Houseguest",
                                MB_ICONERROR);
                    return result;
                }
                return environment->CreateCoreWebView2Controller(
                    g_window, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                                  [](HRESULT r, ICoreWebView2Controller* controller) -> HRESULT {
                                      return on_controller_ready(g_window, r, controller);
                                  })
                                  .Get());
            })
            .Get());

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    g_view.Reset();
    g_controller.Reset();
    CoUninitialize();
    CloseHandle(single);
    return static_cast<int>(message.wParam);
}
