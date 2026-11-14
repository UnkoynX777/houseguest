#include <fstream>
#include <sstream>

#include <catch2/catch_test_macros.hpp>

#include "store.hpp"

using namespace houseguest::service;

namespace {

std::filesystem::path fresh_directory(const char* name) {
    auto dir = std::filesystem::temp_directory_path() / (std::string("hg-test-") + name);
    std::filesystem::remove_all(dir);
    return dir;
}

}  // namespace

TEST_CASE("state round trips through the signed store", "[store]") {
    const auto dir = fresh_directory("roundtrip");
    {
        Store store(dir);
        REQUIRE(store.load() == LoadResult::Fresh);
        store.state.owner_sid = "S-1-5-21-1";
        store.state.profiles = houseguest::core::builtin_profiles();
        store.save();
    }
    Store reopened(dir);
    REQUIRE(reopened.load() == LoadResult::Loaded);
    CHECK(reopened.state.owner_sid == "S-1-5-21-1");
    CHECK(reopened.state.profiles.size() == 5);
    std::filesystem::remove_all(dir);
}

TEST_CASE("a modified state file is rejected and the previous snapshot is used", "[store][security]") {
    const auto dir = fresh_directory("tamper");
    {
        Store store(dir);
        store.load();
        store.state.owner_sid = "S-1-5-21-1";
        store.save();
        store.state.language = "pt-BR";
        store.save();
    }
    {
        std::ifstream in(dir / "state.json");
        std::stringstream buffer;
        buffer << in.rdbuf();
        auto text = buffer.str();
        const auto at = text.find("pt-BR");
        REQUIRE(at != std::string::npos);
        text.replace(at, 5, "xx-XX");
        std::ofstream(dir / "state.json", std::ios::trunc) << text;
    }
    Store reopened(dir);
    CHECK(reopened.load() == LoadResult::Recovered);
    CHECK(reopened.state.language == "en");
    std::filesystem::remove_all(dir);
}

TEST_CASE("tampering with every copy is reported", "[store][security]") {
    const auto dir = fresh_directory("tamper-all");
    {
        Store store(dir);
        store.load();
        store.state.owner_sid = "S-1-5-21-1";
        store.save();
    }
    std::ofstream(dir / "state.json", std::ios::trunc) << "{}";
    Store reopened(dir);
    CHECK(reopened.load() == LoadResult::Tampered);
    CHECK_FALSE(reopened.state.configured());
    std::filesystem::remove_all(dir);
}

TEST_CASE("backups restore verified state only", "[store]") {
    const auto dir = fresh_directory("backup");
    Store store(dir);
    store.load();
    store.state.owner_sid = "S-1-5-21-1";
    store.state.language = "pt-BR";
    store.save();
    const auto backup = store.create_backup().filename().string();
    store.state.language = "en";
    REQUIRE(store.restore_backup(backup));
    CHECK(store.state.language == "pt-BR");
    CHECK_FALSE(store.restore_backup("..\\state.json"));
    CHECK_FALSE(store.restore_backup("missing.json"));
    std::filesystem::remove_all(dir);
}
