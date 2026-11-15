#include <windows.h>

#include <filesystem>

#include <catch2/catch_test_macros.hpp>

#include "houseguest/core/evaluator.hpp"
#include "houseguest/system/com_servers.hpp"
#include "houseguest/system/text.hpp"
#include "houseguest/system/windows_info.hpp"

namespace core = houseguest::core;
namespace system_ = houseguest::system;

namespace {

std::filesystem::path system_directory() {
    wchar_t buffer[MAX_PATH] = {};
    GetSystemDirectoryW(buffer, MAX_PATH);
    return buffer;
}

core::ProcessFacts live_facts(const std::filesystem::path& image, bool com_server,
                              const std::string& parent) {
    core::ProcessFacts facts;
    facts.app.path = system_::path_to_utf8(image);
    facts.app.publisher = system_::trusted_publisher(image);
    facts.publisher_resolved = true;
    facts.hash_resolved = true;
    facts.com_server = com_server;
    facts.parent_path = parent;
    return facts;
}

}  // namespace

TEST_CASE("components this machine's Windows actually ships classify from real evidence",
          "[protection][live]") {
    wchar_t windows[MAX_PATH] = {};
    wchar_t program_files[MAX_PATH] = {};
    GetWindowsDirectoryW(windows, MAX_PATH);
    ExpandEnvironmentStringsW(L"%ProgramFiles%", program_files, MAX_PATH);
    const auto protection = core::Protection::windows_default(system_::path_to_utf8(windows),
                                                              system_::path_to_utf8(program_files));
    const core::Profile profile{"p", "restricted", core::Action::Deny, {}, {}};
    const auto now = std::chrono::system_clock::now();
    const auto svchost = system_::path_to_utf8(system_directory() / "svchost.exe");
    system_::ComServerIndex index;

    const auto compkg = system_directory() / "CompPkgSrv.exe";
    if (std::filesystem::exists(compkg)) {
        const auto decision = core::evaluate(profile, protection, live_facts(compkg, false, ""), now);
        CHECK(decision.action == core::Action::Allow);
        CHECK(decision.reason == "system_infrastructure");
    }

    const auto mobsync = system_directory() / "mobsync.exe";
    if (std::filesystem::exists(mobsync)) {
        const bool registered = index.registered(mobsync);
        const auto by_shell = core::evaluate(profile, protection, live_facts(mobsync, registered, ""), now);
        CHECK(by_shell.action == core::Action::Deny);
        if (registered) {
            const auto by_dcom = core::evaluate(profile, protection, live_facts(mobsync, true, svchost), now);
            CHECK(by_dcom.action == core::Action::Allow);
            CHECK(by_dcom.reason == "windows_com_server");
        }
    }

    for (const auto* name : {"cmd.exe", "powershell.exe", "rundll32.exe"}) {
        const auto tool = system_directory() / name;
        if (!std::filesystem::exists(tool)) continue;
        INFO(name);
        CHECK(core::evaluate(profile, protection, live_facts(tool, index.registered(tool), svchost), now)
                  .action == core::Action::Deny);
    }
}
