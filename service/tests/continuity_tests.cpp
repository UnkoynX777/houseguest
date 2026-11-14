#include <windows.h>

#include <fstream>

#include <catch2/catch_test_macros.hpp>

#include "houseguest/system/crypto.hpp"

#include "continuity.hpp"
#include "lineage.hpp"
#include "store.hpp"

using namespace houseguest::service;
namespace core = houseguest::core;
namespace system_ = houseguest::system;

namespace {

struct Child {
    PROCESS_INFORMATION info{};
    ~Child() {
        if (info.hProcess) {
            TerminateProcess(info.hProcess, 0);
            CloseHandle(info.hProcess);
            CloseHandle(info.hThread);
        }
    }
};

Child start_idle_child() {
    Child child;
    STARTUPINFOW startup{sizeof(startup)};
    std::wstring command = L"cmd.exe /c ping -n 30 127.0.0.1 >nul";
    REQUIRE(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                           nullptr, &startup, &child.info));
    return child;
}

std::filesystem::path fresh_directory(const char* name) {
    auto dir = std::filesystem::temp_directory_path() / (std::string("hg-continuity-") + name);
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

}  // namespace

TEST_CASE("lineage answers only for the process it holds and only for later children", "[lineage]") {
    const auto child = start_idle_child();
    const auto process = system_::ProcessRef::open(child.info.dwProcessId);
    REQUIRE(process);
    const auto created = process->creation_ticks().value();

    Lineage lineage;
    lineage.remember(*process, created, "S-1-5-21-1", {"app-1"});
    CHECK(lineage.size() == 1);

    const auto found = lineage.find(child.info.dwProcessId, created + 1);
    REQUIRE(found);
    CHECK(found->sid == "S-1-5-21-1");
    CHECK(found->members.front() == "app-1");

    CHECK_FALSE(lineage.find(child.info.dwProcessId, created));
    CHECK_FALSE(lineage.find(child.info.dwProcessId, created - 1));
    CHECK_FALSE(lineage.find(child.info.dwProcessId + 4, created + 1));
}

TEST_CASE("lineage merges memberships of the same process and refuses to grow without bound", "[lineage]") {
    const auto child = start_idle_child();
    const auto process = system_::ProcessRef::open(child.info.dwProcessId);
    REQUIRE(process);
    const auto created = process->creation_ticks().value();

    Lineage lineage;
    lineage.remember(*process, created, "S-1-5-21-1", {"a"});
    lineage.remember(*process, created, "S-1-5-21-1", {"b", "a"});
    const auto found = lineage.find(child.info.dwProcessId, created + 1);
    REQUIRE(found);
    CHECK(found->members.size() == 2);

    lineage.clear();
    CHECK(lineage.size() == 0);
    CHECK_FALSE(lineage.find(child.info.dwProcessId, created + 1));
}

TEST_CASE("only the installed executables are Houseguest images", "[continuity]") {
    const auto dir = fresh_directory("images");
    ContinuityGuard guard(dir);
    CHECK(guard.is_houseguest_image(dir / "houseguest.exe"));
    CHECK(guard.is_houseguest_image(dir / "HOUSEGUEST-CLI.EXE"));
    CHECK_FALSE(guard.is_houseguest_image(dir / "notepad.exe"));
    CHECK_FALSE(guard.is_houseguest_image(dir / "sub" / "houseguest.exe"));
    CHECK_FALSE(guard.is_houseguest_image(dir.parent_path() / "houseguest.exe"));
    std::filesystem::remove_all(dir);
}

TEST_CASE("WebView2 images need the runtime location, not just the name", "[continuity]") {
    CHECK(ContinuityGuard::is_webview_image(
        R"(C:\Program Files (x86)\Microsoft\EdgeWebView\Application\130.0.1\msedgewebview2.exe)"));
    CHECK_FALSE(ContinuityGuard::is_webview_image(R"(C:\Users\guest\Downloads\msedgewebview2.exe)"));
    CHECK_FALSE(ContinuityGuard::is_webview_image(
        R"(C:\Program Files (x86)\Microsoft\EdgeWebView\Application\130.0.1\other.exe)"));
}

TEST_CASE("a replaced Houseguest executable no longer verifies", "[continuity][security]") {
    const auto dir = fresh_directory("pinned");
    {
        std::ofstream(dir / "houseguest.exe", std::ios::binary) << "original";
    }
    ContinuityGuard guard(dir);
    guard.pin();
    const auto original =
        system_::to_hex(system_::sha256({reinterpret_cast<const std::uint8_t*>("original"), 8}));
    CHECK_FALSE(guard.houseguest_verified(dir / "houseguest.exe", std::string(64, '0')));
    CHECK_FALSE(guard.houseguest_verified(dir / "houseguest.exe", ""));
    const auto report = guard.report({});
    CHECK(report.missing.size() == 2);
    CHECK(report.pinned == false);
    static_cast<void>(original);
    std::filesystem::remove_all(dir);
}

TEST_CASE("applications and the dependency library survive the signed store", "[store][application]") {
    const auto dir = fresh_directory("library");
    core::Application app;
    app.id = "app-1";
    app.name = "Minecraft";
    app.mode = core::DependencyMode::Related;
    core::Component main;
    main.id = "c-1";
    main.role = core::ComponentRole::Main;
    main.path = "c:/games/mc/launcher.exe";
    main.sha256 = std::string(64, 'a');
    main.publisher = "CN=Mojang AB";
    main.follow_updates = true;
    app.components.push_back(main);
    {
        Store store(dir);
        store.load();
        store.state.owner_sid = "S-1-5-21-1";
        core::Profile profile{"p-1", "Games", core::Action::Deny, {}, {app}};
        store.state.profiles.push_back(profile);
        store.state.library.push_back(app);
        store.save();
    }
    Store reopened(dir);
    REQUIRE(reopened.load() == LoadResult::Loaded);
    REQUIRE(reopened.state.profiles.size() == 1);
    const auto& loaded = reopened.state.profiles.front().apps.front();
    CHECK(loaded.mode == core::DependencyMode::Related);
    CHECK(loaded.components.front().follow_updates);
    CHECK(loaded.components.front().publisher == "CN=Mojang AB");
    CHECK(reopened.state.library.size() == 1);
    std::filesystem::remove_all(dir);
}
