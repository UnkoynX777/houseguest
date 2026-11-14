#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <memory>
#include <string>
#include <thread>

#include "controller.hpp"
#include "pipe_server.hpp"

namespace {

constexpr wchar_t kServiceName[] = L"Houseguest";
constexpr wchar_t kDisplayName[] = L"Houseguest Access Control";
constexpr wchar_t kDescription[] =
    L"Enforces Houseguest guest-session application policies. Stopping it requires administrator rights.";

SERVICE_STATUS_HANDLE g_status_handle = nullptr;
HANDLE g_stop_event = nullptr;
std::filesystem::path g_data_directory;
bool g_enforce = true;

std::filesystem::path default_data_directory() {
    wchar_t buffer[MAX_PATH] = {};
    GetEnvironmentVariableW(L"ProgramData", buffer, MAX_PATH);
    return std::filesystem::path(buffer) / L"Houseguest";
}

void report(DWORD state, DWORD accepted = 0, DWORD wait_hint = 0) {
    SERVICE_STATUS status{};
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = state;
    status.dwControlsAccepted = state == SERVICE_START_PENDING ? 0 : accepted;
    status.dwWaitHint = wait_hint;
    SetServiceStatus(g_status_handle, &status);
}

bool run(HANDLE stop_event, std::string& error) {
    try {
        houseguest::service::Controller controller(g_data_directory, g_enforce);
        houseguest::service::PipeServer server(controller);
        if (!server.start(error)) return false;
        WaitForSingleObject(stop_event, INFINITE);
        server.stop();
        controller.shutdown();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

DWORD WINAPI control_handler(DWORD control, DWORD, LPVOID, LPVOID) {
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN ||
        control == SERVICE_CONTROL_PRESHUTDOWN) {
        report(SERVICE_STOP_PENDING, 0, 10000);
        SetEvent(g_stop_event);
        return NO_ERROR;
    }
    return control == SERVICE_CONTROL_INTERROGATE ? NO_ERROR : ERROR_CALL_NOT_IMPLEMENTED;
}

void WINAPI service_main(DWORD, LPWSTR*) {
    g_status_handle = RegisterServiceCtrlHandlerExW(kServiceName, control_handler, nullptr);
    if (!g_status_handle) return;
    report(SERVICE_START_PENDING, 0, 10000);
    g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    std::thread runner;
    std::string error;
    bool ok = true;
    runner = std::thread([&] { ok = run(g_stop_event, error); });
    report(SERVICE_RUNNING, SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_PRESHUTDOWN);
    runner.join();
    CloseHandle(g_stop_event);

    SERVICE_STATUS status{};
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = SERVICE_STOPPED;
    status.dwWin32ExitCode = ok ? NO_ERROR : ERROR_SERVICE_SPECIFIC_ERROR;
    status.dwServiceSpecificExitCode = ok ? 0 : 1;
    SetServiceStatus(g_status_handle, &status);
}

BOOL WINAPI console_handler(DWORD) {
    SetEvent(g_stop_event);
    return TRUE;
}

int run_console() {
    g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetConsoleCtrlHandler(console_handler, TRUE);
    std::puts("houseguest-service running in console mode. Press Ctrl+C to stop.");
    std::string error;
    if (!run(g_stop_event, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    return 0;
}

int install_service() {
    wchar_t module[MAX_PATH];
    GetModuleFileNameW(nullptr, module, MAX_PATH);
    const std::wstring command = L"\"" + std::wstring(module) + L"\"";

    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!manager) {
        std::fprintf(stderr, "Cannot open the service manager (run as administrator).\n");
        return 1;
    }
    SC_HANDLE service = CreateServiceW(manager, kServiceName, kDisplayName, SERVICE_ALL_ACCESS,
                                       SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                                       command.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
    if (!service) {
        std::fprintf(stderr, "CreateService failed: %lu\n", GetLastError());
        CloseServiceHandle(manager);
        return 1;
    }
    SERVICE_DESCRIPTIONW description{const_cast<LPWSTR>(kDescription)};
    ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, &description);

    SC_ACTION actions[] = {{SC_ACTION_RESTART, 5000}, {SC_ACTION_RESTART, 30000}, {SC_ACTION_RESTART, 60000}};
    SERVICE_FAILURE_ACTIONSW failure{86400, nullptr, nullptr, 3, actions};
    ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS, &failure);

    const bool started = StartServiceW(service, 0, nullptr) != 0;
    std::puts(started ? "Service installed and started." : "Service installed; start it manually.");
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return 0;
}

int uninstall_service() {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) return 1;
    SC_HANDLE service = OpenServiceW(manager, kServiceName, SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);
    if (!service) {
        CloseServiceHandle(manager);
        return GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST ? 0 : 1;
    }
    SERVICE_STATUS status{};
    ControlService(service, SERVICE_CONTROL_STOP, &status);
    for (int i = 0;
         i < 50 && QueryServiceStatus(service, &status) && status.dwCurrentState != SERVICE_STOPPED; ++i)
        Sleep(200);
    const bool deleted = DeleteService(service) != 0;
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    std::puts(deleted ? "Service removed." : "Could not remove the service.");
    return deleted ? 0 : 1;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    g_data_directory = default_data_directory();
    bool console = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--install") return install_service();
        if (arg == L"--uninstall") return uninstall_service();
        if (arg == L"--console")
            console = true;
        else if (arg == L"--no-enforce")
            g_enforce = false;
        else if (arg == L"--data-dir" && i + 1 < argc)
            g_data_directory = argv[++i];
        else {
            std::fprintf(stderr,
                         "usage: houseguest-service [--install | --uninstall | --console [--data-dir DIR] "
                         "[--no-enforce]]\n");
            return 2;
        }
    }
    if (console) return run_console();

    wchar_t name[] = L"Houseguest";
    SERVICE_TABLE_ENTRYW table[] = {{name, service_main}, {nullptr, nullptr}};
    return StartServiceCtrlDispatcherW(table) ? 0 : 1;
}
