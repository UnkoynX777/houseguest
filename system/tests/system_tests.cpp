#include <windows.h>

#include <set>

#include <catch2/catch_test_macros.hpp>

#include "houseguest/system/app_catalog.hpp"
#include "houseguest/system/capabilities.hpp"
#include "houseguest/system/crypto.hpp"
#include "houseguest/system/process.hpp"
#include "houseguest/system/text.hpp"
#include "houseguest/system/windows_info.hpp"

using namespace houseguest::system;

namespace {

Bytes bytes(std::string_view s) {
    return Bytes(s.begin(), s.end());
}

std::filesystem::path system_file(std::string_view pattern) {
    return path_from_utf8(expand_known_folders(pattern));
}

}  // namespace

TEST_CASE("sha256 matches the FIPS 180 test vector", "[crypto]") {
    CHECK(to_hex(sha256(bytes("abc"))) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("hmac-sha256 matches RFC 4231 case 2", "[crypto]") {
    CHECK(to_hex(hmac_sha256(bytes("Jefe"), bytes("what do ya want for nothing?"))) ==
          "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

TEST_CASE("pbkdf2-hmac-sha256 matches RFC 7914 vector", "[crypto]") {
    CHECK(to_hex(pbkdf2_sha256("passwd", bytes("salt"), 1, 32)) ==
          "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc");
}

TEST_CASE("hex parsing rejects malformed input", "[crypto]") {
    CHECK(from_hex("0aFF").has_value());
    CHECK_FALSE(from_hex("0a1").has_value());
    CHECK_FALSE(from_hex("zz").has_value());
}

TEST_CASE("dpapi round trip", "[crypto]") {
    const auto secret = random_bytes(32);
    const auto sealed = dpapi_protect_machine(secret);
    REQUIRE(sealed != secret);
    CHECK(dpapi_unprotect(sealed) == secret);
}

TEST_CASE("known folders are expanded", "[text]") {
    const auto expanded = expand_known_folders(R"(%SystemRoot%\System32\cmd.exe)");
    CHECK(expanded.find('%') == std::string::npos);
    CHECK(expanded.ends_with(R"(System32\cmd.exe)"));
}

TEST_CASE("embedded signatures resolve a trusted publisher", "[signature]") {
    CHECK(trusted_publisher(system_file(R"(%SystemRoot%\explorer.exe)")).find("Microsoft") !=
          std::string::npos);
}

TEST_CASE("catalog-signed windows binaries resolve a trusted publisher", "[signature]") {
    CHECK(
        trusted_publisher(system_file(R"(%SystemRoot%\System32\cmd.exe)")).find("O=Microsoft Corporation") !=
        std::string::npos);
}

TEST_CASE("unsigned files have no publisher", "[signature]") {
    CHECK(trusted_publisher(std::filesystem::path(__FILE__)).empty());
}

TEST_CASE("discovery finds applications with existing paths", "[discovery]") {
    const auto catalog = discover_catalog({});
    REQUIRE_FALSE(catalog.apps.empty());
    CHECK(std::filesystem::exists(path_from_utf8(catalog.apps.front().path)));
    CHECK_FALSE(catalog.sources.empty());
}

TEST_CASE("each executable appears once however many sources list it", "[discovery]") {
    const auto catalog = discover_catalog({});
    std::set<std::string> keys;
    for (const auto& app : catalog.apps) {
        if (!app.path.empty()) CHECK(keys.insert(normalize_app_key(path_from_utf8(app.path))).second);
    }
}

TEST_CASE("installers and helpers are told apart from applications", "[discovery]") {
    CHECK(classify_executable(R"(C:\Apps\Tool\unins000.exe)", "") == AppRole::Installer);
    CHECK(classify_executable(R"(C:\Apps\Tool\vc_redist.x64.exe)", "") == AppRole::Installer);
    CHECK(classify_executable(R"(C:\Apps\Tool\crashpad_handler.exe)", "") == AppRole::Helper);
    CHECK(classify_executable(R"(C:\Apps\Tool\Tool Updater.exe)", "") == AppRole::Helper);
    CHECK(classify_executable(R"(C:\Windows\System32\ping.exe)", "") == AppRole::Tool);
    CHECK(classify_executable(R"(C:\Users\x\AppData\Local\Programs\Microsoft VS Code\Code.exe)", "") ==
          AppRole::Application);
}

TEST_CASE("a portable folder yields its main program per sub folder", "[discovery]") {
    const auto root = std::filesystem::temp_directory_path() / "hg-portable-scan";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "Alpha" / "bin");
    std::filesystem::create_directories(root / "Beta");
    const auto copy = [](const char* name, const std::filesystem::path& to) {
        std::filesystem::copy_file(std::filesystem::path(R"(C:\Windows\System32)") / name, to);
    };
    copy("find.exe", root / "Alpha" / "Alpha.exe");
    copy("find.exe", root / "Alpha" / "unins000.exe");
    copy("find.exe", root / "Alpha" / "bin" / "crashpad_handler.exe");
    copy("find.exe", root / "Beta" / "Beta.exe");
    copy("find.exe", root / "loose.exe");

    const auto apps = scan_folder(root);
    std::set<std::string> names;
    for (const auto& app : apps) {
        names.insert(std::filesystem::path(app.path).filename().string());
        CHECK(app.kind == AppKind::Portable);
    }
    CHECK(names == std::set<std::string>{"Alpha.exe", "Beta.exe", "loose.exe"});
    std::filesystem::remove_all(root);
}

TEST_CASE("current process is introspectable", "[process]") {
    const auto self = ProcessRef::open(static_cast<std::uint32_t>(GetCurrentProcessId()));
    REQUIRE(self.has_value());
    CHECK(self->user_sid().has_value());
    CHECK(self->image_path().has_value());
    CHECK(self->creation_ticks().has_value());
    CHECK(self->creation_ticks().value() <= ProcessRef::now_ticks());
}

TEST_CASE("capability probe reports every check with a verdict", "[capabilities]") {
    const auto capabilities = probe_capabilities();
    CHECK_FALSE(capabilities.build.empty());
    CHECK(capabilities.checks.size() >= 8);
    for (const auto& check : capabilities.checks) {
        CHECK_FALSE(check.id.empty());
        CHECK_FALSE(check.detail.empty());
    }
    CHECK_FALSE(capabilities.critical_failure());
    CHECK(to_json(capabilities)["ready"].get<bool>());
}

TEST_CASE("only local drive-letter paths are opened by the service", "[system][security]") {
    CHECK(houseguest::system::is_local_disk_path(R"(C:\Windows\System32\notepad.exe)"));
    CHECK_FALSE(houseguest::system::is_local_disk_path(R"(\server\share\tool.exe)"));
    CHECK_FALSE(houseguest::system::is_local_disk_path(R"(\?\UNC\server\share\tool.exe)"));
    CHECK_FALSE(houseguest::system::is_local_disk_path(R"(tool.exe)"));
    CHECK_FALSE(houseguest::system::is_local_disk_path(R"(1:\tool.exe)"));
    CHECK_FALSE(houseguest::system::is_local_disk_path(""));
}
