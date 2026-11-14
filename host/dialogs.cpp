#include "dialogs.hpp"

#include <shellapi.h>
#include <shlobj.h>
#include <wrl.h>

#include <fstream>
#include <sstream>

#include "houseguest/system/text.hpp"

#include "WebView2.h"

namespace houseguest::host {
namespace {

using core::Json;
using Microsoft::WRL::ComPtr;
using system::to_wide;

constexpr std::size_t kMaxFileBytes = std::size_t{2} * 1024 * 1024;
constexpr wchar_t kRuntimePage[] = L"https://developer.microsoft.com/microsoft-edge/webview2/";

std::optional<std::wstring> chosen_path(IFileDialog* dialog, HWND owner) {
    if (FAILED(dialog->Show(owner))) return std::nullopt;
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dialog->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        return std::nullopt;
    }
    std::wstring result = path;
    CoTaskMemFree(path);
    return result;
}

}  // namespace

bool is_elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated;
}

Json save_text(HWND owner, const Json& args) {
    const auto name = to_wide(args.value("name", "export.json"));
    const auto content = args.value("content", "");
    ComPtr<IFileSaveDialog> dialog;
    if (FAILED(
            CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        return {{"saved", false}};
    }
    dialog->SetFileName(name.c_str());
    dialog->SetDefaultExtension(L"json");
    const auto path = chosen_path(dialog.Get(), owner);
    if (!path) return {{"saved", false}};
    std::ofstream out(*path, std::ios::binary | std::ios::trunc);
    out << content;
    return {{"saved", static_cast<bool>(out)}};
}

Json open_text(HWND owner) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(
            CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        return {{"opened", false}};
    }
    const COMDLG_FILTERSPEC filter[] = {{L"JSON", L"*.json"}};
    dialog->SetFileTypes(1, filter);
    const auto path = chosen_path(dialog.Get(), owner);
    if (!path) return {{"opened", false}};
    std::ifstream in(*path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    if (buffer.str().size() > kMaxFileBytes) return {{"opened", false}, {"reason", "file too large"}};
    return {{"opened", true}, {"content", buffer.str()}};
}

bool relaunch_elevated() {
    wchar_t module[MAX_PATH];
    GetModuleFileNameW(nullptr, module, MAX_PATH);
    SHELLEXECUTEINFOW info{sizeof(info)};
    info.lpVerb = L"runas";
    info.lpFile = module;
    info.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&info) != FALSE;
}

bool webview_runtime_installed() {
    PWSTR version = nullptr;
    const HRESULT result = GetAvailableCoreWebView2BrowserVersionString(nullptr, &version);
    const bool installed = SUCCEEDED(result) && version != nullptr;
    CoTaskMemFree(version);
    return installed;
}

void offer_webview_runtime(HWND owner, bool portuguese) {
    const wchar_t* text =
        portuguese ? L"O Houseguest precisa do Microsoft Edge WebView2 Runtime, que não foi encontrado neste "
                     L"computador. Abrir a página oficial de download da Microsoft?"
                   : L"Houseguest needs the Microsoft Edge WebView2 Runtime, which was not found on this "
                     L"computer. Open the official Microsoft download page?";
    if (MessageBoxW(owner, text, L"Houseguest", MB_YESNO | MB_ICONINFORMATION) == IDYES) {
        ShellExecuteW(owner, L"open", kRuntimePage, nullptr, nullptr, SW_SHOWNORMAL);
    }
}

}  // namespace houseguest::host
