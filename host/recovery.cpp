#include "recovery.hpp"

#include <chrono>
#include <ctime>
#include <string>

#include "houseguest/core/serialization.hpp"
#include "houseguest/system/pipe.hpp"
#include "houseguest/system/text.hpp"

namespace houseguest::host {
namespace {

using core::Json;
using system::to_utf8;
using system::to_wide;

constexpr wchar_t kClass[] = L"HouseguestRecovery";
constexpr int kStatus = 101, kPin = 102, kEnd = 103, kRefresh = 104, kRestart = 105, kClose = 106,
              kResult = 107;

struct State {
    bool portuguese = false;
    HWND window = nullptr;
    HWND status = nullptr;
    HWND pin = nullptr;
    HWND result = nullptr;
    HFONT font = nullptr;
    RecoveryResult outcome = RecoveryResult::Closed;
    bool done = false;
};

State g;

std::wstring pick(const wchar_t* english, const wchar_t* portuguese) {
    return g.portuguese ? portuguese : english;
}

Json call(const std::string& command, Json args = Json::object(), const std::string& token = {}) {
    system::PipeClient client;
    Json request{{"id", 1}, {"cmd", command}, {"args", std::move(args)}};
    if (!token.empty()) request["token"] = token;
    const auto reply = client.call(request);
    if (!reply) return {{"ok", false}, {"error", {{"code", "service_unavailable"}, {"message", ""}}}};
    return *reply;
}

std::wstring clock_of(std::int64_t seconds) {
    const std::time_t value = static_cast<std::time_t>(seconds);
    std::tm local{};
    localtime_s(&local, &value);
    wchar_t text[32];
    wcsftime(text, 32, L"%H:%M", &local);
    return text;
}

void refresh() {
    const auto reply = call("status");
    std::wstring text;
    if (!reply.value("ok", false)) {
        text = pick(
            L"The Houseguest service is not running.\r\nRestrictions are not being enforced. An "
            L"administrator can start the \"Houseguest\" service from Services.",
            L"O serviço do Houseguest não está em execução.\r\nAs restrições não estão sendo aplicadas. "
            L"Um administrador pode iniciar o serviço \"Houseguest\" em Serviços.");
    } else {
        const Json& data = reply["data"];
        if (data.value("armed", false)) {
            text = pick(L"Protection is ACTIVE", L"Proteção ATIVA") + std::wstring(L"\r\n") +
                   pick(L"Profile: ", L"Perfil: ") + to_wide(data.value("profile", std::string())) + L"\r\n" +
                   pick(L"Ends at ", L"Termina às ") + clock_of(data.value("ends_at", std::int64_t{0}));
        } else {
            text = pick(L"No session is active. Nothing is being restricted.",
                        L"Nenhuma sessão ativa. Nada está sendo restringido.");
        }
        text += L"\r\n" + pick(L"Supervision: ", L"Supervisão: ") +
                to_wide(data.value("enforcement", std::string()));
    }
    SetWindowTextW(g.status, text.c_str());
}

void end_session() {
    wchar_t buffer[128] = {};
    GetWindowTextW(g.pin, buffer, 128);
    const auto pin = to_utf8(buffer);
    SecureZeroMemory(buffer, sizeof(buffer));
    SetWindowTextW(g.pin, L"");
    if (pin.empty()) {
        SetWindowTextW(g.result,
                       pick(L"Type the owner PIN first.", L"Digite primeiro o PIN do proprietário.").c_str());
        return;
    }
    const auto unlocked = call("unlock", {{"pin", pin}});
    if (!unlocked.value("ok", false)) {
        const auto code = unlocked["error"].value("code", std::string());
        SetWindowTextW(g.result,
                       (code == "invalid_pin"    ? pick(L"Incorrect PIN.", L"PIN incorreto.")
                        : code == "rate_limited" ? pick(L"Too many attempts. Wait and try again.",
                                                        L"Muitas tentativas. Aguarde e tente de novo.")
                        : code == "forbidden"    ? pick(L"Only the owner account can do this.",
                                                        L"Somente a conta do proprietário pode fazer isso.")
                                                 : pick(L"The service could not be reached.",
                                                        L"Não foi possível falar com o serviço."))
                           .c_str());
        return;
    }
    const auto token = unlocked["data"].value("token", std::string());
    const auto stopped = call("session.stop", Json::object(), token);
    call("lock", Json::object(), token);
    SetWindowTextW(g.result,
                   (stopped.value("ok", false)
                        ? pick(L"The session was ended.", L"A sessão foi encerrada.")
                        : pick(L"The session could not be ended.", L"Não foi possível encerrar a sessão."))
                       .c_str());
    refresh();
}

HWND control(const wchar_t* type, const std::wstring& text, DWORD style, int x, int y, int w, int h, int id) {
    HWND item = CreateWindowExW(0, type, text.c_str(), WS_CHILD | WS_VISIBLE | style, x, y, w, h, g.window,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    SendMessageW(item, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
    return item;
}

LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_COMMAND:
            switch (LOWORD(wparam)) {
                case kEnd:
                    end_session();
                    return 0;
                case kRefresh:
                    refresh();
                    return 0;
                case kRestart:
                    g.outcome = RecoveryResult::RestartInterface;
                    DestroyWindow(window);
                    return 0;
                case kClose:
                    DestroyWindow(window);
                    return 0;
                default:
                    break;
            }
            break;
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            g.done = true;
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

RecoveryResult show_recovery(HWND owner, bool portuguese) {
    g = State{};
    g.portuguese = portuguese;
    NONCLIENTMETRICSW metrics{sizeof(metrics)};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
    g.font = CreateFontIndirectW(&metrics.lfMessageFont);

    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.lpfnWndProc = proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hIcon = LoadIconW(window_class.hInstance, MAKEINTRESOURCEW(1));
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = kClass;
    RegisterClassExW(&window_class);

    const int width = 480, height = 360;
    RECT area{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
    g.window = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, kClass,
                               pick(L"Houseguest recovery", L"Recuperação do Houseguest").c_str(),
                               WS_CAPTION | WS_SYSMENU, (area.left + area.right - width) / 2,
                               (area.top + area.bottom - height) / 2, width, height, owner, nullptr,
                               window_class.hInstance, nullptr);
    if (!g.window) return RecoveryResult::Closed;

    control(L"STATIC",
            pick(L"This window works without the Houseguest interface. It talks to the protection service "
                 L"directly.",
                 L"Esta janela funciona sem a interface do Houseguest. Ela fala direto com o serviço de "
                 L"proteção."),
            0, 16, 14, 440, 36, 0);
    g.status = control(L"STATIC", L"", SS_LEFT, 16, 58, 440, 76, kStatus);
    control(L"STATIC", pick(L"Owner PIN", L"PIN do proprietário"), 0, 16, 144, 200, 18, 0);
    g.pin =
        control(L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_PASSWORD | ES_AUTOHSCROLL, 16, 164, 280, 24, kPin);
    SendMessageW(g.pin, EM_SETLIMITTEXT, 64, 0);
    control(L"BUTTON", pick(L"End session", L"Encerrar sessão"), WS_TABSTOP | BS_DEFPUSHBUTTON, 308, 162, 148,
            28, kEnd);
    g.result = control(L"STATIC", L"", SS_LEFT, 16, 198, 440, 36, kResult);
    control(L"STATIC",
            pick(L"An administrator can also run \"houseguest-cli recover\" from an elevated prompt.",
                 L"Um administrador também pode executar \"houseguest-cli recover\" em um prompt elevado."),
            0, 16, 240, 440, 36, 0);
    control(L"BUTTON", pick(L"Refresh", L"Atualizar"), WS_TABSTOP, 16, 288, 100, 28, kRefresh);
    control(L"BUTTON", pick(L"Restart interface", L"Reiniciar interface"), WS_TABSTOP, 124, 288, 170, 28,
            kRestart);
    control(L"BUTTON", pick(L"Close", L"Fechar"), WS_TABSTOP, 356, 288, 100, 28, kClose);

    refresh();
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(g.window, SW_SHOW);
    SetForegroundWindow(g.window);
    SetFocus(g.pin);
    MSG message;
    while (!g.done && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(g.window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (owner) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }
    DeleteObject(g.font);
    UnregisterClassW(kClass, window_class.hInstance);
    return g.outcome;
}

}  // namespace houseguest::host
