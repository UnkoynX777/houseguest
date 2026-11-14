# Roadmap

Status: **done** = implemented and verified as described; **planned** = not started.

**Architectural decision:** Houseguest enforces policy with its own process supervision only. It will not use AppLocker or App Control for Business (WDAC), because they behave differently across editions, change machine-wide policy and cannot be relied on in reduced or modified Windows builds. Future work improves the supervisor instead.

## 0.1.0 (done)

Policy engine, supervised enforcement, guest sessions with timer, audit mode and exceptions, signed store, audit chain, authenticated pipe, interface, CLI, MSI and portable zip. Verified on Windows 10 Pro 22H2.

## 0.2.0 (done)

- Window sized to the monitor work area, centered, Per-Monitor DPI v2, saved placement repaired when off screen, responsive layouts down to 760x480
- Windows light/dark theme followed by default with a System / Light / Dark choice and no flash on start
- Notification-area icon with status, session timer, notifications, minimize-to-tray and start with Windows
- Interface redesign on shared components and Lucide icons
- Runtime capability probe, controlled degradation (polling fallback when Event Tracing is unavailable), WebView2 Runtime check with official download offer
- Handle-based process inspection (no PID reuse races), de-duplicated inspection, measured stop latency, automatic recovery of the event session
- Uninstall guard while a guest session is active, documented administrator override
- Fuzz targets (policy, glob, state, IPC), AddressSanitizer build, clang-tidy, dependency review and `npm audit` in CI
- Release pipeline verifies checksums and attestations and attaches a dependency inventory

## Next

| Item | Why it matters |
| --- | --- |
| Windows 11 and other editions test matrix | Only Windows 10 Pro has been exercised |
| Child-process awareness in the supervisor | Show which allowed program launched a blocked one |
| Smarter hash cache and background pre-warming | Faster first launch of large programs under hash or publisher rules |
| Microsoft Store (packaged) app rules by package family | Today packaged apps are matched by executable path or publisher only |
| Code signing for installer and binaries | Removes SmartScreen friction and proves origin |
| App icons in the interface | Currently initials tiles |
| Accessibility audit with a screen reader | Keyboard and ARIA basics exist; not audited |
| Update notifications with signature verification | Today the About page only checks the GitHub API on request |
| Dark native menus for the tray | Windows offers no documented API; the tray menu follows the system menu style |

## Open decisions

- Name clearance (trademark search) for "Houseguest".
- Code-signing certificate source.
