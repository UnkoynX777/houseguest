#include <windows.h>
#include <wincrypt.h>

#include <filesystem>
#include <fstream>
#include <set>

#include <catch2/catch_test_macros.hpp>

#include "houseguest/system/app_catalog.hpp"
#include "houseguest/system/capabilities.hpp"
#include "houseguest/system/com_servers.hpp"
#include "houseguest/system/crypto.hpp"
#include "houseguest/system/process.hpp"
#include "houseguest/system/signature.hpp"
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

TEST_CASE("a COM local server command names its program", "[com]") {
    using houseguest::system::ComServerIndex;
    const std::filesystem::path tool = LR"x(C:\Program Files\Tool\tool.exe)x";
    const std::filesystem::path mobsync = LR"x(C:\Windows\System32\mobsync.EXE)x";
    const auto quoted = ComServerIndex::executable_of(LR"x("C:\Program Files\Tool\tool.exe" /Embedding)x");
    const auto bare = ComServerIndex::executable_of(LR"x(C:\Program Files\Tool\tool.exe /Embedding)x");
    const auto upper = ComServerIndex::executable_of(LR"x(C:\Windows\System32\mobsync.EXE -Embedding)x");
    CHECK(quoted == tool);
    CHECK(bare == tool);
    CHECK(upper == mobsync);
    CHECK(ComServerIndex::executable_of(L"   ").empty());
    CHECK(ComServerIndex::executable_of(L"").empty());
}

TEST_CASE("the COM server index reads the machine registrations once and reuses them", "[com]") {
    houseguest::system::ComServerIndex index;
    const std::filesystem::path unregistered = LR"x(C:\Users\guest\Downloads\not-registered.exe)x";
    CHECK(index.size() > 50);
    CHECK(index.rebuilds() == 1);
    CHECK_FALSE(index.registered(unregistered));
    CHECK(index.size() > 50);
    CHECK(index.rebuilds() == 1);
}


TEST_CASE("a file stamp changes when content is replaced even if size and write time are restored", "[stamp][safety]") {
    const auto file = std::filesystem::temp_directory_path() / L"houseguest-stamp-test.bin";
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << "AAAA";
    }
    const auto before = houseguest::system::file_stamp(file);
    REQUIRE(before.has_value());
    CHECK(before->size == 4);
    CHECK(houseguest::system::file_stamp(file) == before);

    Sleep(60);  // file times advance with the system clock tick, about 16 ms
    HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(handle != INVALID_HANDLE_VALUE);
    DWORD written = 0;
    REQUIRE(WriteFile(handle, "BBBB", 4, &written, nullptr));
    FILETIME restored;
    restored.dwLowDateTime = static_cast<DWORD>(before->written & 0xFFFFFFFF);
    restored.dwHighDateTime = static_cast<DWORD>(before->written >> 32);
    REQUIRE(SetFileTime(handle, nullptr, nullptr, &restored));
    CloseHandle(handle);

    const auto after = houseguest::system::file_stamp(file);
    REQUIRE(after.has_value());
    CHECK(after->size == before->size);
    CHECK(after->written == before->written);
    CHECK(after->changed != before->changed);
    CHECK_FALSE(after == before);

    std::error_code ec;
    std::filesystem::remove(file, ec);
    CHECK_FALSE(houseguest::system::file_stamp(file).has_value());
}

namespace {

std::filesystem::path scratch_file(const wchar_t* name) {
    const auto folder = std::filesystem::temp_directory_path() / L"houseguest-signature-tests";
    std::filesystem::create_directories(folder);
    return folder / name;
}

}  // namespace

TEST_CASE("a file Windows vouches for through a catalog reads as a valid signature with a readable signer",
          "[signature]") {
    const auto report = inspect_signature(system_file("%SystemRoot%\\System32\\kernel32.dll"));
    REQUIRE(report.state == SignatureState::Valid);
    CHECK(report.signer == "Microsoft Windows");
    CHECK(report.issuer.common_name.find("Microsoft") != std::string::npos);
    CHECK(report.signer.find("OID") == std::string::npos);
    CHECK(report.signer.find('=') == std::string::npos);
    CHECK(report.thumbprint_sha1.size() == 40);
    CHECK(report.thumbprint_sha256.size() == 64);
    CHECK(report.not_after > report.not_before);
    CHECK_FALSE(report.revocation_checked);
}

TEST_CASE("an unsigned file is reported as unsigned, not as an error", "[signature]") {
    const auto file = scratch_file(L"plain.exe");
    std::ofstream(file, std::ios::binary | std::ios::trunc) << "MZ not signed";
    const auto report = inspect_signature(file);
    CHECK(report.state == SignatureState::Unsigned);
    CHECK(report.signer.empty());
    CHECK(report.thumbprint_sha1.empty());
    CHECK(trusted_publisher(file).empty());
}

TEST_CASE("a file that cannot be read is unverified, not unsigned", "[signature]") {
    const auto report = inspect_signature(scratch_file(L"does-not-exist.exe"));
    CHECK(report.state == SignatureState::Unverified);
}

TEST_CASE("a signed file that was modified keeps its certificate but is not valid", "[signature][safety]") {
    // Find any file with an embedded signature in the system folder, copy it and change one byte of its code.
    std::filesystem::path donor;
    std::error_code ec;
    int looked = 0;
    for (std::filesystem::directory_iterator it(system_file("%ProgramFiles%\\Windows Defender"), ec), end;
         it != end && !ec && looked < 40; it.increment(ec)) {
        if (it->path().extension() != L".exe") continue;
        ++looked;
        const auto report = inspect_signature(it->path());
        if (report.state == SignatureState::Valid && !report.via_catalog) {
            donor = it->path();
            break;
        }
    }
    if (donor.empty()) {
        WARN("no embedded-signature program found on this machine; the check was not run");
        return;
    }
    const auto copy = scratch_file(L"modified.exe");
    std::filesystem::copy_file(donor, copy, std::filesystem::copy_options::overwrite_existing);
    {
        std::fstream file(copy, std::ios::binary | std::ios::in | std::ios::out);
        const auto size = std::filesystem::file_size(copy);
        file.seekg(static_cast<std::streamoff>(size / 3));
        char byte = 0;
        file.read(&byte, 1);
        byte = static_cast<char>(byte ^ 0x5a);
        file.seekp(static_cast<std::streamoff>(size / 3));
        file.write(&byte, 1);
    }
    const auto report = inspect_signature(copy);
    CHECK(report.state == SignatureState::Invalid);
    CHECK_FALSE(report.signer.empty());
    CHECK(trusted_publisher(copy).empty());
    CHECK(trusted_publisher(donor) != "");
}

TEST_CASE("distinguished names keep every attribute but only friendly ones name the signer", "[signature]") {
    const wchar_t* text = L"CN=\"Example Games, Inc.\", O=\"Example Games, Inc.\", C=US, "
                          L"1.3.6.1.4.1.311.60.2.1.3=US, 2.5.4.15=Private Organization, 2.5.4.5=1234567";
    DWORD size = 0;
    CertStrToNameW(X509_ASN_ENCODING, text, CERT_X500_NAME_STR, nullptr, nullptr, &size, nullptr);
    std::vector<BYTE> encoded(size);
    REQUIRE(CertStrToNameW(X509_ASN_ENCODING, text, CERT_X500_NAME_STR, nullptr, encoded.data(), &size, nullptr));
    const auto name = parse_distinguished_name(encoded);
    CHECK(name.common_name == "Example Games, Inc.");
    CHECK(name.organization == "Example Games, Inc.");
    CHECK(name.country == "US");
    REQUIRE(name.fields.size() == 6);
    const auto jurisdiction = std::ranges::find(name.fields, "1.3.6.1.4.1.311.60.2.1.3", &NameField::oid);
    REQUIRE(jurisdiction != name.fields.end());
    CHECK(jurisdiction->label == "Jurisdiction country");
    CHECK(name.fields.front().label == "Serial number");
    CHECK(parse_distinguished_name({}).fields.empty());
    CHECK(parse_distinguished_name(bytes("not der")).fields.empty());
}
