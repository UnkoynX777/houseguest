# Architecture

## Decisions

| Concern | Choice | Reason |
| --- | --- | --- |
| Language | C++23 (MSVC) | Deterministic resources, direct Win32 access |
| Build | CMake + Ninja | Works from the command line and CI without Visual Studio project files |
| Policy engine | Portable C++ with no Windows headers | Unit-testable and fuzzable anywhere; enforced by `check-repo-hygiene.ps1` |
| Windows layer | Win32, CNG, DPAPI, ETW, WinVerifyTrust, named pipes | Official APIs, no framework |
| Interface | Native WebView2 host + Vue 3 + TypeScript | See below |
| JSON | nlohmann/json (pinned commit) | Strict parsing, widely reviewed |
| Tests | Catch2 (pinned commit) | Small, good CI output |

### Why WebView2 and Vue instead of WinUI 3 or Qt

WinUI 3 needs MSBuild, the Windows App SDK toolchain and packaging, which makes command-line builds and contributions harder. Qt adds a large dependency and LGPL obligations for an interface that is open only briefly. WebView2 uses the evergreen runtime already on the machine and gives a mature ecosystem for design systems, internationalization and accessibility. The cost is memory in the interface process. It is accepted because the interface is not needed for enforcement: it starts on demand and exits, and the service holds the policy.

## Processes

```
houseguest.exe (WebView2 host, unelevated)  --\
  Vue 3 app <-> host bridge                     \
houseguest-cli.exe                          ------ named pipe \\.\pipe\houseguest ----> houseguest-service.exe (LocalSystem)
```

- The interface never enforces. Closing it changes nothing.
- The service is event driven: ETW process-start events feed two worker threads, a condition variable wakes the session timer at the deadline, and the pipe server blocks on connections. A maintenance thread runs a low-frequency safety sweep, restarts a lost event session and switches to one-second polling when events are unavailable.
- The host is per user. It owns the window, the notification-area icon and a small monitor thread that polls the service status every three seconds to update the tooltip and raise notifications. It only forwards messages from its own origin, blocks navigation elsewhere, and exposes native helpers (elevate, save and open file, interface preferences).
- Exiting or closing the interface never changes protection. Interface preferences (theme, tray behaviour, window placement) live in ui.json inside the Houseguest folder of the user local app data.

## Modules

| Module | Responsibility |
| --- | --- |
| `core` | Types, glob and signer matching, the protection table, evaluation, analysis (conflicts, shadowing, protected overlap, application and component checks), simulation, JSON serialization, built-in profiles |
| `system` | SHA-256, HMAC, PBKDF2, DPAPI, signature and catalog verification, app discovery, process references (one handle per inspection), session helpers, capability probe, pipe client |
| `service` | Signed store, audit chain, identity resolver (cached hashes and publishers), ETW session, assessor (facts for one process), lineage table, continuity guard, observation, supervisor (workers, de-duplication, ordered retries, polling fallback, statistics), controller (commands split by domain, tokens, sessions), pipe server, SCM entry point |
| `cli` | Status, setup, recovery, scripted calls |
| `host` | WebView2 window and bridge, window geometry and DPI, notification-area icon, service status monitor, interface preferences, native dialogs, recovery console, icon extraction |
| `fuzz` | libFuzzer targets for policies, globs, the state parser and the command handler |
| `ui` | Vue application and design system |

Dependencies point downward. `core` knows nothing about Windows.

## Policy semantics

The engine (`core/evaluator`) decides from a `ProcessFacts` record that the platform layer fills in; it never reads the system. The order is fixed and the result is deterministic:

1. **Protection.** A process the kernel marks as critical (`IsProcessCritical`) or protected is never denied, whatever its path. After that comes the compiled-in table of Windows components (`core/protection.cpp`): exact locations under the Windows folder for sign-in, services, the desktop shell, input and accessibility, plus signed programs in driver packages (`DriverStore\FileRepository`) and signed system apps. System32 as a whole is not protected, and no file name protects a file outside its directory. The table is reviewed code, not configuration, and a unit test pins its size.
2. **Continuity.** Houseguest's own programs (hash pinned when the service starts, installed in a folder the restricted accounts cannot write) and the Microsoft-signed WebView2 runtime processes started below a verified Houseguest process, from a runtime folder those accounts cannot write.
3. **Candidates**, ranked by priority, then Deny over Allow, then source (rule, application, dependency, related), then id: profile rules, application executables, approved components and related descendants.
4. The profile **default**.

A decision that needs a hash or signature that was not resolved yet says so (`needs_hash`, `needs_publisher`) and the caller resolves it once and asks again, so expensive checks only run when the outcome depends on them. Every decision carries a reason code, the rule or application and component that produced it, and notes such as a component that changed since it was approved.

### Applications and dependencies

An application has components: main executables, launchers, helpers, runtimes, updaters, services. A component is identified by a path or package plus an optional hash, signer, or both, and can follow updates signed by the pinned signer. Its scope is `descendant` (allowed only when an allowed member of the application is its parent) or `independent` (started by Windows or a broker, which requires a hash or signer pin). The application mode selects what its executable may start: only itself, its approved components, or also unlisted descendants that share its signer (or, for unsigned applications, live in its folder), never tools that run other programs (`is_sensitive_tool`). Unknown descendants are blocked and queued for review.

The service remembers which allowed processes belong to which application in a lineage table. Each record holds a handle to its process, so the system cannot give that process id to another program while the record exists, and a child's parent id is trusted only when the parent is older than the child and the owner, session and exit time agree. Membership is never inherited by a process that was not itself allowed.

### Application library

The library lists what can be allowed, and is separate from what the engine enforces: the engine decides by file identity, the library only finds files and shows their effective decision under the selected profile.

`system/app_catalog` builds the list from complementary sources, each timed and isolated so one unavailable source never stops the others:

- the Uninstall registry (64-bit and 32-bit machine views, plus every loaded user hive, which is where per-user installers such as VS Code and Discord register),
- App Paths of the machine and of each loaded user,
- Start menu shortcuts of all users and of every profile folder,
- packaged apps read from the manifests under `WindowsApps` (newest version per package family),
- conventional install folders (Program Files, per-user `Programs`) for folders no other source already explains,
- winget, Scoop and Chocolatey folders,
- PATH entries from the machine and user environment, classified as tools,
- folders registered as portable and programs added by hand.

Entries are merged by canonical long path, never by name, so one executable found by several sources appears once with all its sources. Executables are classified as application, tool, helper or installer from their name and location, and tools and helpers are hidden by default. Nothing is executed during discovery. The whole disk is never scanned: only the sources above, one or two levels inside install folders, and folders the owner registers.

The service keeps the result in `catalog.json`, so the library answers at once after a restart and refreshes in a background thread of below-normal priority. A refresh requested while another runs is queued once. First-seen times feed the "found recently" filter; the first scan only sets a baseline. `apps.diagnose` re-checks one path step by step (existence, extension, local disk, readability, catalog membership) and says whether it is missing, filtered or a duplicate.

Limits: other users' registry hives that are not loaded are not read; their programs are found through their profile folders (Start menu, `Programs`, package-manager folders). Windows services and drivers are not listed as applications.

### Observation

The owner can watch one application run for up to 30 minutes. The service records the owner's processes started below it, or kept in its folder, with hash, signer and the evidence the owner needs (unsigned, outside the folder, writable location, tool that runs programs). Nothing is allowed by observation; the owner picks what to save.

### Recovery path

`houseguest.exe` opens a native window that talks to the service directly (tray menu, `--recovery`, or when WebView2 fails), `houseguest-cli` administers from a prompt, and `houseguest-cli recover` ends a session without the PIN from an elevated prompt. Before a session starts, the preview checks that the Houseguest folder cannot be written by the restricted accounts, that its programs are pinned and that the policy does not block them.

`analyze()` reports errors that block activation (malformed matchers and components, duplicate ids, an application without a main executable, an independent component without a pin) and warnings that need confirmation (equal-priority conflicts, rules overlapping protected components, an application covered by a stronger Deny, sensitive tools approved as components, no allowances).

## Authentication and authorization

| Command class | Requirement |
| --- | --- |
| `ping`, `status`, `unlock` | Any local user. `unlock` additionally requires the caller to be the owner account and the correct PIN. |
| `setup`, `recover` | Caller token must be elevated. `setup` works only while no owner exists. |
| Everything else | A token issued by `unlock`, bound to the owner SID, valid 15 minutes, extended on use. |

The pipe has an explicit DACL, rejects remote clients and uses overlapped I/O with deadlines: 5 seconds for a first message, 10 seconds to finish a message, 120 seconds idle and 5 seconds per write. Connections are limited per user (four open, three still waiting for a first message) and globally (24 open, 16 waiting), the peer is accounted for from the moment it connects, messages are capped at 1 MiB with a JSON nesting limit of 32, and each connection has a request rate limit. The caller identity is read from the pipe's impersonation token, never from the message. Owner tokens live in memory under their own lock.

## Persistence

`%ProgramData%\Houseguest` is restricted to SYSTEM and Administrators with a protected DACL. `state.json` carries an HMAC-SHA256 over its content with a key sealed by DPAPI (machine scope). Writes are atomic with a `state.previous.json` snapshot; a file that fails verification is set aside and the previous snapshot is used. `audit.log` is append-only JSON lines. Each record is sealed with an HMAC-SHA256 that covers the previous record's seal, keyed from the same DPAPI-protected secret, so the chain cannot be recomputed without that key. A small keyed anchor file records the last sequence and seal to reveal truncation, files rotate at 2 MiB (eight kept), and a partly written final record is moved aside on startup. A local administrator can read the key, so this detects tampering by anyone else and makes administrator tampering harder to hide; it does not stop an administrator.

## Session lifecycle

The session (profile, guests, deadline, exceptions) is persisted, so a service restart or reboot resumes it, and a session that expired while the service was stopped ends on the next start with the configured action. A session ends in two phases: it is first marked as ending (persisted), the expiry action runs, and only when that succeeds is the policy lifted. If the action fails, for example because the session list cannot be read, the policy keeps applying and the service retries every ten seconds. An administrator can always force the end with `houseguest-cli recover`.

Guests are existing Windows accounts, which Houseguest never disables or alters, or accounts created by Houseguest (`hg-` prefix, recorded as managed). Managed accounts are created disabled, enabled for a session with a Windows account expiry equal to the session end (so Windows itself refuses logons after the deadline even if the service is down), disabled and logged off when the session ends, and re-disabled by the service at startup if they are not part of an active session. Disabling is only ever applied to accounts in the managed list.

## Process supervision

Each process-start event is queued (events first, reconciliation sweeps second, 8192 entries each), inspected through one handle, and tracked per process id and creation time as in flight, retrying or settled. Transient failures (handle, token, image path, hash, a termination that fails while the process is still alive) are retried with backoff from 25 ms to 1 s up to seven times, and a failure never counts as permission: when retries are exhausted it is reported and the decision uses what is known (path only, for a missing hash). Dropped events are counted and trigger an immediate reconciliation of running processes. Files on network locations are never opened by the service, so a guest cannot make a SYSTEM process authenticate to a server it chooses; hash and publisher rules cannot be evaluated for such programs and the audit log says so. The package family comes from the process token.

## Performance

Measured numbers and how to reproduce them are in [performance.md](performance.md).
