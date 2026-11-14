#include <fstream>
#include <sstream>

#include <catch2/catch_test_macros.hpp>

#include "houseguest/system/crypto.hpp"

#include "audit.hpp"

using namespace houseguest::service;
namespace hgsys = houseguest::system;

namespace {

hgsys::Bytes test_key(std::uint8_t seed = 7) {
    return hgsys::Bytes(32, seed);
}

std::filesystem::path fresh_directory(const char* name) {
    const auto directory = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    return directory;
}

std::string read_file(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

void write_file(const std::filesystem::path& file, const std::string& text) {
    std::ofstream(file, std::ios::binary | std::ios::trunc) << text;
}

}  // namespace

TEST_CASE("audit chain verifies and detects edits", "[audit][security]") {
    const auto directory = fresh_directory("hg-audit-edit");
    const auto file = directory / "audit.log";
    {
        AuditLog log(file, test_key());
        log.append("owner", "session.started", {{"profile", "guest"}});
        log.append("supervisor", "app.blocked", {{"path", "c:/x.exe"}});
        log.append("owner", "session.ended");
        CHECK(log.verify_chain());
        CHECK(log.tail(2).size() == 2);
        CHECK(log.tail(2)[0]["event"] == "session.ended");
    }
    {
        AuditLog reopened(file, test_key());
        reopened.append("owner", "unlock");
        CHECK(reopened.verify_chain());
        CHECK(reopened.tail(10).size() == 4);
    }

    auto text = read_file(file);
    const auto at = text.find("app.blocked");
    REQUIRE(at != std::string::npos);
    text.replace(at, 11, "app.allowed");
    write_file(file, text);
    CHECK_FALSE(AuditLog(file, test_key()).verify_chain());
    std::filesystem::remove_all(directory);
}

TEST_CASE("removing an entry breaks the chain", "[audit][security]") {
    const auto directory = fresh_directory("hg-audit-remove");
    const auto file = directory / "audit.log";
    {
        AuditLog log(file, test_key());
        for (int i = 0; i < 3; ++i) log.append("a", "e");
    }
    std::ifstream in(file);
    std::string first, second, third;
    std::getline(in, first);
    std::getline(in, second);
    std::getline(in, third);
    in.close();
    write_file(file, first + "\n" + third + "\n");
    CHECK_FALSE(AuditLog(file, test_key()).verify_chain());
    std::filesystem::remove_all(directory);
}

TEST_CASE("recomputing the chain without the key is detected", "[audit][security]") {
    const auto directory = fresh_directory("hg-audit-forge");
    const auto file = directory / "audit.log";
    {
        AuditLog log(file, test_key());
        log.append("owner", "session.started");
        log.append("supervisor", "app.blocked", {{"path", "c:/x.exe"}});
    }
    AuditLog attacker_view(file, test_key(9));
    CHECK_FALSE(attacker_view.verify_chain());

    std::string previous;
    std::string forged;
    std::istringstream lines(read_file(file));
    std::string line;
    while (std::getline(lines, line)) {
        auto entry = houseguest::core::Json::parse(line);
        if (entry["event"] == "app.blocked") entry["event"] = "app.allowed";
        entry.erase("hash");
        entry["prev"] = previous;
        const auto body = previous + entry.dump();
        previous =
            hgsys::to_hex(hgsys::sha256({reinterpret_cast<const std::uint8_t*>(body.data()), body.size()}));
        entry["hash"] = previous;
        forged += entry.dump() + "\n";
    }
    write_file(file, forged);
    CHECK_FALSE(AuditLog(file, test_key()).verify_chain());
    std::filesystem::remove_all(directory);
}

TEST_CASE("truncating the log is reported", "[audit][security]") {
    const auto directory = fresh_directory("hg-audit-truncate");
    const auto file = directory / "audit.log";
    {
        AuditLog log(file, test_key());
        for (int i = 0; i < 5; ++i) log.append("a", "e");
    }
    std::ifstream in(file);
    std::string first, second;
    std::getline(in, first);
    std::getline(in, second);
    in.close();
    write_file(file, first + "\n" + second + "\n");

    AuditLog reopened(file, test_key());
    const auto report = reopened.verify();
    CHECK(reopened.tail(50).front()["event"] == "audit.integrity_failure");
    CHECK(report.truncated);

    std::filesystem::remove(file);
    CHECK(AuditLog(file, test_key()).verify().truncated);
    std::filesystem::remove_all(directory);
}

TEST_CASE("a torn final record is quarantined and the chain continues", "[audit][reliability]") {
    const auto directory = fresh_directory("hg-audit-torn");
    const auto file = directory / "audit.log";
    {
        AuditLog log(file, test_key());
        log.append("a", "first");
        log.append("a", "second");
    }
    std::ofstream(file, std::ios::binary | std::ios::app) << "{\"v\":2,\"seq\":3,\"ts\":1,\"act";

    AuditLog reopened(file, test_key());
    reopened.append("a", "third");
    const auto report = reopened.verify();
    CHECK(report.valid);
    CHECK(report.entries == 3);
    bool quarantined = false;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        quarantined |= entry.path().filename().string().find("torn") != std::string::npos;
    }
    CHECK(quarantined);
    std::filesystem::remove_all(directory);
}

TEST_CASE("rotation keeps one verifiable chain", "[audit][reliability]") {
    const auto directory = fresh_directory("hg-audit-rotate");
    const auto file = directory / "audit.log";
    {
        AuditLog log(file, test_key());
        const std::string filler(8000, 'x');
        for (int i = 0; i < 300; ++i) log.append("a", "event", {{"blob", filler}});
        CHECK(log.verify().valid);
        CHECK(log.tail(3).size() == 3);
    }
    CHECK(std::filesystem::exists(directory / "audit.log.1"));
    AuditLog reopened(file, test_key());
    reopened.append("a", "after");
    const auto report = reopened.verify();
    CHECK(report.valid);
    CHECK(report.last_sequence == 301);
    std::filesystem::remove_all(directory);
}

TEST_CASE("oversized detail is replaced instead of growing the log", "[audit][reliability]") {
    const auto directory = fresh_directory("hg-audit-large");
    AuditLog log(directory / "audit.log", test_key());
    log.append("a", "e", {{"blob", std::string(200000, 'y')}});
    CHECK(log.verify().valid);
    CHECK(log.tail(1)[0]["detail"]["truncated"] == true);
    std::filesystem::remove_all(directory);
}

TEST_CASE("logs written before keyed sealing keep verifying", "[audit][compatibility]") {
    const auto directory = fresh_directory("hg-audit-legacy");
    const auto file = directory / "audit.log";
    std::string previous;
    std::string text;
    for (int sequence = 1; sequence <= 3; ++sequence) {
        houseguest::core::Json entry{{"seq", sequence},
                                     {"ts", 100 + sequence},
                                     {"actor", "svc"},
                                     {"event", "old"},
                                     {"detail", houseguest::core::Json::object()},
                                     {"prev", previous}};
        const auto body = previous + entry.dump();
        previous =
            hgsys::to_hex(hgsys::sha256({reinterpret_cast<const std::uint8_t*>(body.data()), body.size()}));
        entry["hash"] = previous;
        text += entry.dump() + "\n";
    }
    write_file(file, text);

    AuditLog log(file, test_key());
    CHECK(log.verify().valid);
    log.append("svc", "new");
    const auto report = log.verify();
    CHECK(report.valid);
    CHECK(report.entries == 4);
    std::filesystem::remove_all(directory);
}
