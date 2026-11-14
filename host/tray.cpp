#include "tray.hpp"

#include <shellapi.h>

#include <algorithm>

#include "houseguest/system/text.hpp"

namespace houseguest::host {
namespace {

constexpr UINT kTrayId = 1;

std::wstring pick(bool portuguese, const wchar_t* english, const wchar_t* portuguese_text) {
    return portuguese ? portuguese_text : english;
}

std::wstring clock_text(std::int64_t seconds) {
    const auto hours = seconds / 3600;
    const auto minutes = (seconds % 3600) / 60;
    wchar_t buffer[32];
    if (hours > 0) {
        swprintf_s(buffer, L"%lld:%02lld", static_cast<long long>(hours), static_cast<long long>(minutes));
    } else {
        swprintf_s(buffer, L"%lld min",
                   static_cast<long long>(std::max<std::int64_t>(minutes, seconds > 0 ? 1 : 0)));
    }
    return buffer;
}

void copy_text(wchar_t* destination, std::size_t capacity, const std::wstring& text) {
    wcsncpy_s(destination, capacity, text.c_str(), _TRUNCATE);
}

}  // namespace

TrayText describe_status(const ServiceStatus& status, std::int64_t remaining_seconds, bool portuguese) {
    TrayText text;
    if (!status.reachable) {
        text.headline = pick(portuguese, L"Service not running", L"Serviço não está em execução");
        text.detail =
            pick(portuguese, L"Protection is not enforced.", L"A proteção não está sendo aplicada.");
    } else if (!status.configured) {
        text.headline = pick(portuguese, L"Setup required", L"Configuração necessária");
        text.detail = pick(portuguese, L"Open Houseguest to create the owner PIN.",
                           L"Abra o Houseguest para criar o PIN do proprietário.");
    } else if (status.armed) {
        text.headline = pick(portuguese, L"Protection active", L"Proteção ativa");
        const auto profile = system::to_wide(status.profile);
        const auto left = clock_text(remaining_seconds);
        text.detail = portuguese ? L"Perfil " + profile + L" · restam " + left
                                 : L"Profile " + profile + L" · " + left + L" left";
        if (!status.enforcement_available) {
            text.detail = pick(portuguese, L"Enforcement unavailable. Open Diagnostics.",
                               L"Aplicação indisponível. Abra Diagnóstico.");
        }
    } else {
        text.headline = pick(portuguese, L"No active session", L"Nenhuma sessão ativa");
        text.detail = pick(portuguese, L"Guests are not restricted.", L"Convidados não estão restritos.");
    }
    text.tooltip = L"Houseguest — " + text.headline;
    if (status.armed && status.reachable) text.tooltip += L"\n" + text.detail;
    return text;
}

std::wstring session_ended_message(bool portuguese) {
    return pick(portuguese, L"The guest session has ended.", L"A sessão de convidado terminou.");
}

std::wstring service_lost_message(bool portuguese) {
    return pick(portuguese, L"The Houseguest service is not responding. Restrictions are not being enforced.",
                L"O serviço do Houseguest não está respondendo. As restrições não estão sendo aplicadas.");
}

std::wstring service_back_message(bool portuguese) {
    return pick(portuguese, L"The Houseguest service is running again.",
                L"O serviço do Houseguest voltou a funcionar.");
}

std::wstring enforcement_lost_message(bool portuguese) {
    return pick(portuguese, L"Process supervision is unavailable during an active session.",
                L"A supervisão de processos está indisponível durante uma sessão ativa.");
}

std::wstring tray_hint_message(bool portuguese) {
    return pick(portuguese, L"Houseguest keeps running in the notification area. Protection is not affected.",
                L"O Houseguest continua na área de notificação. A proteção não é afetada.");
}

Tray::Tray(HWND owner, UINT callback_message)
    : owner_(owner),
      callback_message_(callback_message),
      icon_(static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON,
                                          GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0))) {
}

Tray::~Tray() {
    remove();
    if (icon_) DestroyIcon(icon_);
}

NOTIFYICONDATAW Tray::data() const {
    NOTIFYICONDATAW data{sizeof(data)};
    data.hWnd = owner_;
    data.uID = kTrayId;
    return data;
}

bool Tray::add() {
    auto icon = data();
    icon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    icon.uCallbackMessage = callback_message_;
    icon.hIcon = icon_;
    copy_text(icon.szTip, std::size(icon.szTip), L"Houseguest");
    if (!Shell_NotifyIconW(NIM_ADD, &icon)) return false;
    icon.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &icon);
    added_ = true;
    return true;
}

void Tray::remove() {
    if (!added_) return;
    auto icon = data();
    Shell_NotifyIconW(NIM_DELETE, &icon);
    added_ = false;
}

void Tray::set_tooltip(const std::wstring& text) {
    if (!added_) return;
    auto icon = data();
    icon.uFlags = NIF_TIP | NIF_SHOWTIP;
    copy_text(icon.szTip, std::size(icon.szTip), text);
    Shell_NotifyIconW(NIM_MODIFY, &icon);
}

void Tray::balloon(const std::wstring& title, const std::wstring& text, bool error) {
    if (!added_) return;
    auto icon = data();
    icon.uFlags = NIF_INFO;
    icon.dwInfoFlags = error ? NIIF_WARNING : NIIF_INFO;
    copy_text(icon.szInfoTitle, std::size(icon.szInfoTitle), title);
    copy_text(icon.szInfo, std::size(icon.szInfo), text);
    Shell_NotifyIconW(NIM_MODIFY, &icon);
}

std::optional<TrayCommand> Tray::show_menu(const TrayText& text, const TrayMenuState& state) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return std::nullopt;
    const bool pt = state.portuguese;
    AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, text.headline.c_str());
    AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, text.detail.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, static_cast<UINT_PTR>(TrayCommand::Open) + 1,
                pick(pt, L"Open Houseguest", L"Abrir o Houseguest").c_str());
    AppendMenuW(menu, MF_STRING, static_cast<UINT_PTR>(TrayCommand::Settings) + 1,
                pick(pt, L"Settings", L"Configurações").c_str());
    AppendMenuW(menu, MF_STRING, static_cast<UINT_PTR>(TrayCommand::Recovery) + 1,
                pick(pt, L"Recovery console", L"Console de recuperação").c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(
        menu, MF_STRING | (state.minimize_to_tray ? MF_CHECKED : 0),
        static_cast<UINT_PTR>(TrayCommand::ToggleMinimizeToTray) + 1,
        pick(pt, L"Minimize to the notification area", L"Minimizar para a área de notificação").c_str());
    AppendMenuW(menu, MF_STRING | (state.autostart ? MF_CHECKED : 0),
                static_cast<UINT_PTR>(TrayCommand::ToggleAutostart) + 1,
                pick(pt, L"Start with Windows", L"Iniciar com o Windows").c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, static_cast<UINT_PTR>(TrayCommand::Exit) + 1,
                pick(pt, L"Exit (protection keeps running)", L"Sair (a proteção continua)").c_str());
    SetMenuDefaultItem(menu, static_cast<UINT>(TrayCommand::Open) + 1, FALSE);

    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(owner_);
    const UINT chosen = static_cast<UINT>(TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, cursor.x, cursor.y, 0, owner_, nullptr));
    PostMessageW(owner_, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (chosen == 0) return std::nullopt;
    return static_cast<TrayCommand>(chosen - 1);
}

}  // namespace houseguest::host
