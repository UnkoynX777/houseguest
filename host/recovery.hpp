#pragma once

#include <windows.h>

namespace houseguest::host {

enum class RecoveryResult { Closed, RestartInterface };

// A small native window that talks to the service directly. It shows the protection state and lets the owner
// end the session with the PIN, so administration never depends on the WebView2 runtime or on the page.
RecoveryResult show_recovery(HWND owner, bool portuguese);

}  // namespace houseguest::host
