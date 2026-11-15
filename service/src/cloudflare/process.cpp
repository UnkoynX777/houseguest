#include "process.hpp"

#include <windows.h>
#include <sddl.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <thread>

#include "houseguest/system/signature.hpp"

namespace houseguest::service::cloudflare {
namespace {

constexpr const char* kDownload =
    "https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-windows-amd64.exe";

struct HandleCloser {
    void operator()(HANDLE h) const {
        if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
};
using Handle = std::unique_ptr<void, HandleCloser>;

std::wstring quote(const std::wstring& argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\"") == std::wstring::npos) return argument;
    std::wstring out = L"\"";
    for (const auto c : argument) {
        if (c == L'"') out += L'\\';
        out += c;
    }
    return out + L"\"";
}

class WindowsProcess final : public RunningProcess {
public:
    WindowsProcess(Handle process, Handle job, Handle read, LineSink sink)
        : process_(std::move(process)), job_(std::move(job)), read_(std::move(read)), sink_(std::move(sink)) {
        reader_ = std::thread([this] { pump(); });
    }
    ~WindowsProcess() override { stop(); }

    bool alive() override { return WaitForSingleObject(process_.get(), 0) == WAIT_TIMEOUT; }
    void wait(std::chrono::milliseconds limit) override {
        WaitForSingleObject(process_.get(), static_cast<DWORD>(limit.count()));
    }
    void stop() override {
        if (stopped_.exchange(true)) return;
        TerminateJobObject(job_.get(), 0);
        WaitForSingleObject(process_.get(), 5000);
        read_.reset();
        if (reader_.joinable()) reader_.join();
    }

private:
    void pump() {
        std::string pending;
        char buffer[2048];
        for (;;) {
            DWORD n = 0;
            const HANDLE h = read_.get();
            if (!h || !ReadFile(h, buffer, sizeof(buffer), &n, nullptr) || n == 0) break;
            pending.append(buffer, n);
            std::size_t newline;
            while ((newline = pending.find('\n')) != std::string::npos) {
                auto line = pending.substr(0, newline);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                pending.erase(0, newline + 1);
                if (!line.empty()) sink_(line);
            }
            if (pending.size() > 8192) pending.clear();
        }
    }

    Handle process_, job_, read_;
    LineSink sink_;
    std::thread reader_;
    std::atomic<bool> stopped_ = false;
};

}  // namespace

Launcher windows_launcher() {
    return [](const LaunchSpec& spec, LineSink sink) -> std::unique_ptr<RunningProcess> {
        SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
        HANDLE read = nullptr, write = nullptr;
        if (!CreatePipe(&read, &write, &inherit, 0)) throw std::runtime_error("Could not create a pipe.");
        Handle read_end(read), write_end(write);
        SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);

        Handle job(CreateJobObjectW(nullptr, nullptr));
        if (!job) throw std::runtime_error("Could not create a job object.");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits));

        std::wstring command = quote(spec.executable.wstring());
        for (const auto& argument : spec.arguments) {
            const int n = MultiByteToWideChar(CP_UTF8, 0, argument.data(), static_cast<int>(argument.size()), nullptr, 0);
            std::wstring wide(static_cast<std::size_t>(n), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, argument.data(), static_cast<int>(argument.size()), wide.data(), n);
            command += L" " + quote(wide);
        }

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        startup.hStdOutput = write;
        startup.hStdError = write;
        startup.hStdInput = INVALID_HANDLE_VALUE;
        PROCESS_INFORMATION info{};
        if (!CreateProcessW(spec.executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                            CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP, nullptr,
                            spec.directory.empty() ? nullptr : spec.directory.c_str(), &startup, &info))
            throw std::runtime_error("Windows could not start cloudflared (" + std::to_string(GetLastError()) + ").");
        Handle process(info.hProcess), thread(info.hThread);
        if (!AssignProcessToJobObject(job.get(), process.get())) {
            TerminateProcess(process.get(), 1);
            throw std::runtime_error("Could not place cloudflared in a job object.");
        }
        ResumeThread(thread.get());
        write_end.reset();
        return std::make_unique<WindowsProcess>(std::move(process), std::move(job), std::move(read_end), std::move(sink));
    };
}

bool is_official_cloudflared(const std::filesystem::path& file, std::string* detail) {
    const auto report = system::inspect_signature(file);
    auto say = [&](std::string text) {
        if (detail) *detail = std::move(text);
        return false;
    };
    if (report.state != system::SignatureState::Valid) return say(std::string("Signature: ") + system::state_name(report.state));
    std::string organization = report.subject.organization + " " + report.subject.common_name;
    std::ranges::transform(organization, organization.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (organization.find("cloudflare") == std::string::npos) return say("The file is not signed by Cloudflare.");
    if (detail) *detail = report.subject.organization;
    return true;
}

std::optional<std::filesystem::path> locate_cloudflared(const std::vector<std::filesystem::path>& folders) {
    for (const auto& folder : folders) {
        const auto candidate = folder / L"cloudflared.exe";
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec) && is_official_cloudflared(candidate)) return candidate;
    }
    return std::nullopt;
}

bool restrict_to_administrators(const std::filesystem::path& folder) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", SDDL_REVISION_1,
                                                              &descriptor, nullptr))
        return false;
    std::unique_ptr<void, decltype(&LocalFree)> guard(descriptor, &LocalFree);
    return SetFileSecurityW(folder.c_str(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                            descriptor) != 0;
}

bool install_cloudflared(const Transport& transport, const std::filesystem::path& folder, std::string& error) {
    system::HttpsRequest request;
    request.url = kDownload;
    request.timeout_ms = 60000;
    request.max_response = 128u * 1024 * 1024;
    const auto response = transport(request);
    if (!response.ok() || response.body.size() < 1024 * 1024) {
        error = response.error.empty() ? "The download did not complete." : response.error;
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    restrict_to_administrators(folder);
    const auto staged = folder / L"cloudflared.download";
    {
        std::ofstream out(staged, std::ios::binary | std::ios::trunc);
        out.write(response.body.data(), static_cast<std::streamsize>(response.body.size()));
        if (!out) {
            error = "The file could not be written.";
            return false;
        }
    }
    // The signature check needs the .exe extension to be meaningful for some Windows versions; verify the staged copy.
    const auto verified = folder / L"cloudflared.verify.exe";
    std::filesystem::rename(staged, verified, ec);
    std::string detail;
    if (!is_official_cloudflared(verified, &detail)) {
        std::filesystem::remove(verified, ec);
        error = "The downloaded file was rejected: " + detail;
        return false;
    }
    const auto target = folder / L"cloudflared.exe";
    std::filesystem::remove(target, ec);
    std::filesystem::rename(verified, target, ec);
    if (ec) {
        error = "The file could not be installed.";
        return false;
    }
    return true;
}

}  // namespace houseguest::service::cloudflare
