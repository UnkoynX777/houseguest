#pragma once

#include <windows.h>

#include "houseguest/core/serialization.hpp"

namespace houseguest::host {

bool is_elevated();
core::Json save_text(HWND owner, const core::Json& args);
core::Json open_text(HWND owner);
core::Json pick_paths(HWND owner, const core::Json& args);
bool relaunch_elevated();
bool webview_runtime_installed();
void offer_webview_runtime(HWND owner, bool portuguese);

}  // namespace houseguest::host
