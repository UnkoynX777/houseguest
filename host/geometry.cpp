#include "geometry.hpp"

#include <shellscalingapi.h>

#include <algorithm>

namespace houseguest::host {
namespace {

constexpr int kPreferredWidth = 1240;
constexpr int kPreferredHeight = 800;
constexpr int kMinimumWidth = 760;
constexpr int kMinimumHeight = 480;
constexpr int kWorkAreaShare = 92;

RECT work_area(HMONITOR monitor) {
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(monitor, &info);
    return info.rcWork;
}

int width_of(const RECT& r) {
    return r.right - r.left;
}

int height_of(const RECT& r) {
    return r.bottom - r.top;
}

int scale(int value, UINT dpi) {
    return MulDiv(value, static_cast<int>(dpi), 96);
}

UINT monitor_dpi(HMONITOR monitor) {
    UINT x = 96, y = 96;
    if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &x, &y))) return 96;
    return x;
}

RECT centered(const RECT& area, int width, int height) {
    const int left = area.left + (width_of(area) - width) / 2;
    const int top = area.top + (height_of(area) - height) / 2;
    return {left, top, left + width, top + height};
}

HMONITOR monitor_under_cursor() {
    POINT cursor{};
    GetCursorPos(&cursor);
    return MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
}

}  // namespace

RECT preferred_bounds(const RECT& area, UINT dpi) {
    const int width = std::min(scale(kPreferredWidth, dpi), width_of(area) * kWorkAreaShare / 100);
    const int height = std::min(scale(kPreferredHeight, dpi), height_of(area) * kWorkAreaShare / 100);
    return centered(area, width, height);
}

RECT clamp_to(const RECT& area, const RECT& bounds) {
    const int width = std::min(width_of(bounds), width_of(area));
    const int height = std::min(height_of(bounds), height_of(area));
    const int left = std::clamp(static_cast<int>(bounds.left), static_cast<int>(area.left),
                                static_cast<int>(area.right) - width);
    const int top = std::clamp(static_cast<int>(bounds.top), static_cast<int>(area.top),
                               static_cast<int>(area.bottom) - height);
    return {left, top, left + width, top + height};
}

SIZE minimum_for(const RECT& area, UINT dpi) {
    return {std::min<LONG>(scale(kMinimumWidth, dpi), width_of(area)),
            std::min<LONG>(scale(kMinimumHeight, dpi), height_of(area))};
}

InitialWindow initial_window(const std::optional<SavedPlacement>& saved) {
    if (saved) {
        const RECT wanted{saved->x, saved->y, saved->x + saved->width, saved->y + saved->height};
        if (HMONITOR monitor = MonitorFromRect(&wanted, MONITOR_DEFAULTTONULL)) {
            return {clamp_to(work_area(monitor), wanted), saved->maximized};
        }
    }
    HMONITOR monitor = monitor_under_cursor();
    return {preferred_bounds(work_area(monitor), monitor_dpi(monitor)), false};
}

SIZE minimum_size(HWND window) {
    return minimum_for(work_area(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST)),
                       GetDpiForWindow(window));
}

std::optional<SavedPlacement> capture_placement(HWND window) {
    WINDOWPLACEMENT placement{sizeof(placement)};
    if (!GetWindowPlacement(window, &placement)) return std::nullopt;
    const RECT& r = placement.rcNormalPosition;
    return SavedPlacement{r.left, r.top, width_of(r), height_of(r), placement.showCmd == SW_SHOWMAXIMIZED};
}

}  // namespace houseguest::host
