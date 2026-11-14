#include <fstream>
#include <sstream>

#include <catch2/catch_test_macros.hpp>

#include "audit.hpp"

using namespace houseguest::service;

TEST_CASE("audit chain verifies and detects edits", "[audit][security]") {
    const auto file = std::filesystem::temp_directory_path() / "hg-test-audit.log";
    std::filesystem::remove(file);
    {
        AuditLog log(file);
        log.append("owner", "session.started", {{"profile", "guest"}});
        log.append("supervisor", "app.blocked", {{"path", "c:/x.exe"}});
        log.append("owner", "session.ended");
        CHECK(log.verify_chain());
        CHECK(log.tail(2).size() == 2);
        CHECK(log.tail(2)[0]["event"] == "session.ended");
    }
    {
        AuditLog reopened(file);
        reopened.append("owner", "unlock");
        CHECK(reopened.verify_chain());
        CHECK(reopened.tail(10).size() == 4);
    }

    std::ifstream in(file);
    std::stringstream buffer;
    buffer << in.rdbuf();
    auto text = buffer.str();
    const auto at = text.find("app.blocked");
    REQUIRE(at != std::string::npos);
    text.replace(at, 11, "app.allowed");
    in.close();
    std::ofstream(file, std::ios::trunc) << text;
    CHECK_FALSE(AuditLog(file).verify_chain());
    std::filesystem::remove(file);
}

TEST_CASE("removing an entry breaks the chain", "[audit][security]") {
    const auto file = std::filesystem::temp_directory_path() / "hg-test-audit2.log";
    std::filesystem::remove(file);
    {
        AuditLog log(file);
        for (int i = 0; i < 3; ++i) log.append("a", "e");
    }
    std::ifstream in(file);
    std::string first, second, third;
    std::getline(in, first);
    std::getline(in, second);
    std::getline(in, third);
    in.close();
    std::ofstream(file, std::ios::trunc) << first << '\n' << third << '\n';
    CHECK_FALSE(AuditLog(file).verify_chain());
    std::filesystem::remove(file);
}
