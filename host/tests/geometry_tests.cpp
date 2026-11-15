#include <catch2/catch_test_macros.hpp>

#include "../geometry.hpp"

using namespace houseguest::host;

namespace {
int width(const RECT& r) {
    return r.right - r.left;
}
int height(const RECT& r) {
    return r.bottom - r.top;
}
bool inside(const RECT& outer, const RECT& inner) {
    return inner.left >= outer.left && inner.top >= outer.top && inner.right <= outer.right &&
           inner.bottom <= outer.bottom;
}
}  // namespace

TEST_CASE("preferred bounds fit the work area at every common scale") {
    const RECT areas[] = {{0, 0, 1920, 1040}, {0, 0, 1366, 728}, {0, 0, 1280, 680}, {0, 0, 2560, 1400}};
    for (const UINT dpi : {96u, 120u, 144u, 192u}) {
        for (const auto& area : areas) {
            const RECT bounds = preferred_bounds(area, dpi);
            CHECK(inside(area, bounds));
            CHECK(width(bounds) > 0);
            CHECK(height(bounds) > 0);
        }
    }
}

TEST_CASE("preferred bounds are centered") {
    const RECT area{0, 0, 1920, 1040};
    const RECT bounds = preferred_bounds(area, 96);
    CHECK(bounds.left - area.left == area.right - bounds.right);
    CHECK(bounds.top - area.top == area.bottom - bounds.bottom);
}

TEST_CASE("a small screen at 150 percent never gets a window larger than 94 percent of the work area") {
    const RECT area{0, 0, 1366, 728};
    const RECT bounds = preferred_bounds(area, 144);
    CHECK(width(bounds) <= 1366 * 94 / 100);
    CHECK(height(bounds) <= 728 * 94 / 100);
}

TEST_CASE("saved bounds that are off screen are pulled back into view") {
    const RECT area{0, 0, 1920, 1040};
    const RECT clamped = clamp_to(area, {3000, 2000, 4200, 2800});
    CHECK(inside(area, clamped));
    CHECK(width(clamped) == 1200);
    CHECK(height(clamped) == 800);
}

TEST_CASE("saved bounds larger than the work area are reduced") {
    const RECT area{0, 0, 1280, 680};
    const RECT clamped = clamp_to(area, {-50, -50, 2000, 1500});
    CHECK(inside(area, clamped));
    CHECK(width(clamped) == 1280);
    CHECK(height(clamped) == 680);
}

TEST_CASE("the minimum size never exceeds the work area") {
    const SIZE minimum = minimum_for({0, 0, 640, 400}, 192);
    CHECK(minimum.cx <= 640);
    CHECK(minimum.cy <= 400);
    const SIZE normal = minimum_for({0, 0, 1920, 1040}, 96);
    CHECK(normal.cx == 1000);
    CHECK(normal.cy == 700);
}

TEST_CASE("the window has one logical size of 1000 by 700 and keeps it between runs") {
    const RECT area{0, 0, 2560, 1400};
    const RECT bounds = preferred_bounds(area, 96);
    CHECK(width(bounds) == 1000);
    CHECK(height(bounds) == 700);
    const RECT scaled = preferred_bounds(area, 144);
    CHECK(width(scaled) == 1500);
    const auto fixed = minimum_for(area, 96);
    CHECK(fixed.cx == 1000);
    CHECK(fixed.cy == 700);
}

TEST_CASE("on a small work area the fixed size shrinks to fit instead of being cut off") {
    const RECT area{0, 0, 1280, 680};
    const auto fixed = minimum_for(area, 96);
    CHECK(fixed.cx <= 1280);
    CHECK(fixed.cy <= 680 * 94 / 100);
}
