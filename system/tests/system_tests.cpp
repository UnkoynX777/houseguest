#include <windows.h>

#include <catch2/catch_test_macros.hpp>

#include "houseguest/system/crypto.hpp"
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
    const auto apps = discover_installed_apps();
    REQUIRE_FALSE(apps.empty());
    CHECK(std::filesystem::exists(path_from_utf8(apps.front().path)));
}

TEST_CASE("current process is introspectable", "[process]") {
    const auto pid = static_cast<std::uint32_t>(GetCurrentProcessId());
    CHECK(process_user_sid(pid).has_value());
    CHECK(process_image_path(pid).has_value());
}
