# Changelog

All notable changes are documented here. The project follows [Semantic Versioning](https://semver.org/) and the format of [Keep a Changelog](https://keepachangelog.com/).

## [Unreleased]

## [0.4.1]

### Fixed
- The History page was blank when the log held an entry without detail (owner setup and unlock events were stored with a null detail), because the page read a field of null while rendering. The service now always writes and returns an object detail, and the page tolerates old entries.
- The History page hid service errors behind an empty list. It now shows the real error with a retry button, a designed empty state, and a request timeout so a lost reply cannot leave it loading forever.
- Profile cards overflowed their column and cut buttons. Cards now flow in a responsive grid, keep one primary action and move the rest into a menu, and no page scrolls sideways down to 480 px wide.

### Added
- History records the programs a session allowed (once per program and session, never for Windows components that are only left running), shows the program icon, outcome, profile, rule and reason, and opens a technical detail view with the review action for blocks.
- `audit.query` pages the log newest first with event, period and text filters, so the page loads fifty entries at a time.
- Profiles can be renamed from the card menu.

### Changed
- Neutral, flatter design tokens, smaller type, thinner scrollbars that follow the theme, and a window caption and background that match the interface in both themes.

## [0.4.0]

### Fixed
- A restricted account that ran the Houseguest window lost its WebView2 processes, because only the install folder was allowed. The engine now recognizes Houseguest's own programs by pinned hash from a folder restricted accounts cannot write, and WebView2 by its Microsoft signature, runtime folder and a verified Houseguest parent, never by name alone.
- Signer comparison never matched the subject Windows reports, because attributes were compared as text in a different order. Subjects are now compared as attribute sets, and stored profiles with the old Windows signer rule are upgraded to a rule that needs the signer and the Windows folder together, so signed Windows tools copied elsewhere do not run.
- Parent process ids came from a cached snapshot that could predate a reused process id, and retries were not woken early, which delayed evaluation by seconds.
- Components that Windows needs in a user session (driver package helpers, input and accessibility tools, system apps) were blocked in restrictive profiles. A reviewed table now protects them, together with processes the kernel marks as critical or protected, and the kernel flags are checked again before any termination.

### Added
- Applications with explicit dependencies, scoped to their application and pinned by hash or signer, with modes (only the program, approved dependencies, related programs) and a review queue for unknown dependencies.
- Observation of an application run to find what it needs, with the evidence for each program, and saved dependency sets.
- A visual application library with real icons, grid and list views, filters, categories, sorting, multi-select and a detail panel with a live explanation, dependencies, block history, advanced settings and a simulation.
- Block history with the reason, profile, parent, application involved and a recommended action, and a review dialog that approves a dependency or allows a program temporarily.
- Live editing of the active profile, a continuity check before a session starts, a native recovery console that works without WebView2, and the commands `continuity`, `blocks`, `explain` and `reviews` in the command line.
- Discovery of Microsoft Store apps and installed programs, with version and description.
- `scripts/e2e-continuity.ps1`, an isolated end-to-end check that runs a private service instance (builds with `HOUSEGUEST_INSTANCES`) beside an installed one, in audit mode.

### Changed
- Built-in profiles deny a larger set of tools that run other programs, and a Custom template is available.
- Notifications for blocked dependencies are coalesced per application.

## [0.3.0]

### Security
- Pipe server: deadlines on every read and write, limits on pending and open connections per user and overall, request rate limiting, a JSON nesting limit and early peer accounting. Idle or slow clients can no longer hold the service's connections.
- Supervisor: a failed or incomplete inspection is no longer recorded as handled. Explicit per-process states with bounded retries and backoff, retry of terminations that fail on live processes, an audit event for failures that exhaust retries, and counters in Diagnostics.
- Event queue: events and reconciliation sweeps use separate queues, drops are counted, and a drop triggers an immediate reconciliation of running processes. Queue depth and high-water mark are reported.
- Package family rules now take effect: the family name is read from the process.
- Audit log: records are sealed with an HMAC keyed from the DPAPI-protected secret, truncation is detected through a keyed anchor, logs rotate, a torn final record is quarantined, and oversized details are replaced. Older logs keep verifying.
- Sessions end in two phases and keep their policy until the expiry action succeeds. A failed resume after a restart no longer leaves guests unrestricted.
- The service no longer opens files on network locations (running programs and Start Menu shortcuts), which removes a way for a guest to make the SYSTEM process authenticate to another server.
- Owner token storage is now protected by its own lock (it was accessed without one by a few commands).
- Repeated denied unlock attempts and internal errors are rate limited in the audit log so they cannot push older records out of the retained history.
- The service removes unused privileges, restricts DLL search paths and enables process mitigations. WebView2 denies permissions, frames, host objects, dialogs and autofill, and bounds in-flight requests.

### Added
- Managed guest accounts: Houseguest can create standard accounts (prefix `hg-`) that stay disabled outside sessions and expire at the session end by Windows account expiry. The expiry action `disable` is only available for these accounts; existing accounts are never touched.
- Diagnostics: audit status, pipe statistics, retry and drop counters.
- `scripts/e2e-security.ps1`, a libFuzzer target for the audit format and nesting check, installer permission checks, unit tests for limits, audit forgery, truncation, torn records and rotation.

### Changed
- Hash computation no longer blocks other inspections of unrelated files.
- Paths are normalized to long names before matching.


## [0.2.0]

### Added
- Notification-area icon with protection status, profile and time remaining, notifications for session end and service problems, minimize to tray, and optional start with Windows.
- Runtime capability probe shown in Diagnostics. Sessions refuse to start when a critical component is missing or when process supervision is not running.
- Polling fallback when Event Tracing for Windows is unavailable, automatic recovery of the event session, and an audit entry on every mode change.
- Stop-latency, inspection and termination statistics in Diagnostics.
- WebView2 Runtime check with an offer to open Microsoft's official download page.
- Uninstall guard: the installer refuses to remove Houseguest during an active guest session unless an administrator passes `FORCE_UNINSTALL=1`.
- Fuzz targets (policies, globs, state, command handler), AddressSanitizer build, clang-tidy, dependency review and `npm audit` in CI.
- Website on GitHub Pages, FAQ, compatibility, troubleshooting, privacy, use cases and performance documents.

### Changed
- Window is sized to the monitor work area, centered, Per-Monitor DPI v2 aware, remembers its placement and repairs positions that are off screen. Layouts adapt down to 760x480.
- The interface follows the Windows light or dark theme by default (System, Light, Dark), without a flash on start. New design system components and Lucide icons.
- Processes are inspected through a single handle, which removes process-id reuse races. Inspection is de-duplicated and the service reacts in about a millisecond after the process-start event on the test machine.
- Decision: Houseguest will not use AppLocker or App Control for Business. The roadmap now improves the built-in supervisor instead.
- Release pipeline verifies `SHA256SUMS.txt` (LF line endings), attestations, publishes the changelog section and a dependency inventory.
- Dependencies updated: Vue 3.5.45, Vite 6.4, vue-tsc 3, TypeScript 5.9, pinned GitHub Actions.

### Fixed
- Initial window could be larger than the usable screen at 125% to 200% scaling.
- A denial whose termination failed was reported as blocked.

### Known limitations
- Enforcement is reactive.
- Packaged (Microsoft Store) apps are matched by executable path or publisher only.
- Builds are not code signed.
- Validated on Windows 10 Pro 22H2 only. Windows 11, other editions and modified Windows builds are untested.

## [0.1.0]

First public release.

### Added
- Policy engine: path, hash, publisher and package matchers, priorities, temporary rules, conflict and lockout analysis, simulation.
- Windows service (LocalSystem) with process supervision through ETW, guest sessions with a timer, audit mode and temporary exceptions.
- Signed, atomically written state store with backups; hash-chained audit log; PIN with PBKDF2 and persistent back-off.
- Authenticated named-pipe protocol with per-command authorization and administrator-only recovery.
- WebView2 + Vue 3 interface: dashboard, sessions, applications, profiles, rules, history, security, diagnostics, settings, about. English and Portuguese, light and dark.
- `houseguest-cli` for status, setup, recovery and scripted use.
- MSI installer and portable zip.
