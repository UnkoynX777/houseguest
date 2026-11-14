#pragma once

#include <windows.h>

#include <optional>

#include "ui_settings.hpp"

namespace houseguest::host {

struct InitialWindow {
    RECT bounds;
    bool maximized;
};

RECT preferred_bounds(const RECT& work_area, UINT dpi);
RECT clamp_to(const RECT& work_area, const RECT& bounds);
SIZE minimum_for(const RECT& work_area, UINT dpi);
InitialWindow initial_window(const std::optional<SavedPlacement>& saved);
SIZE minimum_size(HWND window);
std::optional<SavedPlacement> capture_placement(HWND window);

}  // namespace houseguest::host
