#include <catch2/catch_test_macros.hpp>

#include "houseguest/core/glob.hpp"

using namespace houseguest::core;

TEST_CASE("path patterns match case-insensitively with either slash", "[glob]") {
    CHECK(path_matches(R"(C:\Games\Minecraft\javaw.exe)", "c:/games/minecraft/JAVAW.EXE"));
    CHECK_FALSE(path_matches("c:/games/minecraft/javaw.exe", "c:/games/minecraft/java.exe"));
}

TEST_CASE("single star stays within a segment", "[glob]") {
    CHECK(path_matches("c:/apps/*/run.exe", "c:/apps/foo/run.exe"));
    CHECK_FALSE(path_matches("c:/apps/*/run.exe", "c:/apps/foo/bar/run.exe"));
    CHECK(path_matches("c:/apps/*.exe", "c:/apps/tool.exe"));
    CHECK(path_matches("c:/apps/t?ol.exe", "c:/apps/tool.exe"));
}

TEST_CASE("double star spans segments including none", "[glob]") {
    CHECK(path_matches("c:/windows/**", "c:/windows/system32/cmd.exe"));
    CHECK(path_matches("c:/windows/**", "c:/windows"));
    CHECK(path_matches("c:/a/**/z.exe", "c:/a/z.exe"));
    CHECK(path_matches("c:/a/**/z.exe", "c:/a/b/c/z.exe"));
    CHECK_FALSE(path_matches("c:/a/**/z.exe", "c:/b/z.exe"));
}

TEST_CASE("invalid patterns never match", "[glob]") {
    CHECK_FALSE(is_valid_path_pattern("relative/path.exe"));
    CHECK_FALSE(is_valid_path_pattern("c:/a/**/**/**/**/**/x"));
    CHECK_FALSE(path_matches("relative/*.exe", "c:/x.exe"));
    CHECK_FALSE(path_matches("c:/**", "relative.exe"));
}

TEST_CASE("pathological patterns terminate", "[glob]") {
    const std::string path = "c:/" + std::string(200, 'a') + "/x";
    CHECK_FALSE(path_matches("c:/*a*a*a*a*a*a*a*a*b", path));
}
