# Changelog

All notable changes are documented here. The project follows [Semantic Versioning](https://semver.org/) and the format of [Keep a Changelog](https://keepachangelog.com/).

## [Unreleased]

### Fixed
- Opening the Blocks tab of a program made the detail panel disappear while it stayed open internally, so only Escape recovered it. Audit events of some kinds (a process the kernel protects) carry no note, and reading it raised an error that removed the whole panel. Events are now read into a fixed shape, a tab that cannot be drawn fails alone with a retry, and a failed or slow load no longer leaves placeholders forever or empties the panel on reload.
- Panels, dialogs and menus now share one layer system: they are drawn above everything regardless of page containers, only the top one answers Escape, the page behind is unreachable while one is open, the focus returns to where it came from, and a page left in the background no longer keeps its dialogs. Stacking values are tokens. See docs/interface-layers.md.
- The chip counts of the Applications page now follow the platform, drive and search in use, and an empty list offers to clear the filters.
- Programs whose installation record names an icon file, such as VALORANT, showed initials instead of their icon. Discovery now collects every place a picture can be read from (the record's `DisplayIcon` with its resource index, shortcut icons, package and Xbox game logos) and the window chooses the best genuine one by kind and size, falling back to the executable's own resources and only then to initials. Install folders written with forward slashes produced paths the shell could not open; paths are now normalised where they enter the catalog.
- The signer shown for a program was the raw certificate subject, with OIDs and every attribute. The overview now says who signed it, and the advanced view lists issuer, validity, serial, thumbprints and each attribute with its label.
- A certificate being present was shown as if the signature were good. Signatures now have separate states: valid, not trusted, expired, invalid (modified or revoked), not signed and could not be verified; a modified file still names its original signer but reads as invalid.
- Long paths, hashes and thumbprints overflowed or were cut off with no way to read them; they now wrap or are limited to a few lines with the full text on hover, and have copy and show-in-folder buttons.
- Windows components that Windows itself starts were treated like any other program and could be stopped under a restrictive profile, among them the component package server, the first-run experience broker, Sync Center and the BitLocker prompt. System infrastructure is now recognised by its location together with the Windows signing identity, and Windows-signed programs that an administrator registered as COM servers are recognised when the DCOM launcher starts them. A program that only sits in System32, or is signed by someone else, is still decided by the profile, and administrative tools are never granted by this rule.
- A scan of the library took 18 s and a refresh with nothing changed 23 s because every path was resolved again for every entry; it now takes 2.4 s, and the library answers in 8 ms instead of 215 ms.
- A file replaced under the same name, size and write time kept its cached hash and signer. The identity cache now also compares the file system's change time and the file id.
- Returning to Applications, Rules, History or any other page reset its filters, view mode and scroll position and reloaded everything.
- The library missed whole classes of programs: per-user installs, games whose registration names only a folder and an icon, Steam games on other drives, and entries whose executable differs from the product name. Discovery now builds installations from independent sources and resolves the main program from evidence, with no depth limit and no name matching.
- Executables inside an install folder are found with a time and entry budget, never following junctions or symlinks, so deep layouts such as `game\bin\win64` no longer depend on a fixed depth.
- Packaged apps are read from `WindowsApps` manifests, and uninstall entries without an install folder are correlated with install folders by name.

### Added
- `docs/metadata.md`: the four kinds of information (visual, declared, verified, effective), which source wins for each field and why, how icons are chosen, what the signature states mean and their limits, and measurements. Entries now record where their name, publisher and version came from, and keep values from lower-ranked sources as alternatives.
- Detail view in two layers: type, version, summarised location and signature up front; file, discovery source, values declared by the program and the digital signature in an expandable section. Declared values are labelled as unverified; absent values say whether they are not declared, not recorded or not available.
- `apps.details` (version resource and signature, remembered by file stamp), `host.reveal`, and `scripts/survey-icons.ps1` to see where every picture on a machine comes from.
- Cache and invalidation architecture (see `docs/caching.md`): per-page data that goes stale when the service reports a new policy, catalog or history generation, a library that refreshes when the machine changes (watching the Start menu, program folders, volumes, packaged apps, Steam manifests, registered portable folders and the Uninstall keys), and decisions for the library kept for one revision of the stored policy.
- Placeholders shaped like the content, shown only when a page has nothing to display, with a windowed list for Applications.
- Versioned, validated view preferences: last page, profile, filters, sort, view mode and history filters survive restarts; search text, selections and dialogs do not.
- Diagnostics shows decision reuse, watched places, page cache size and the slowest requests; blocked Windows-signed programs are marked in History so they can be allowed.
- Providers for Steam (libraries on every drive, app manifests and launch configuration), Epic, GOG, Ubisoft Connect, EA, Xbox games, winget, Scoop and Chocolatey, each reporting what it found, what only it found, what merged and what it set aside.
- Resolution states: entries whose main program is ambiguous or missing stay in the library with ranked candidates and the owner can choose the executable.
- Dynamic volume enumeration, offline volumes kept and marked, a setting for removable drives, and an opt-in deep scan.
- Library tools: source and drive report, deep scan, add application, add by folder, portable folders, and a diagnosis that also explains discarded candidates and ambiguous installations.
- Platform and drive filters, and entries that need a program choice.

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
