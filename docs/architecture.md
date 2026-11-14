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
| `core` | Types, glob matching, evaluation, analysis (conflicts, shadowing, baseline overlap, missing allowances), simulation, JSON serialization, built-in profiles |
| `system` | SHA-256, HMAC, PBKDF2, DPAPI, signature and catalog verification, app discovery, process references (one handle per inspection), session helpers, capability probe, pipe client |
| `service` | Signed store, audit chain, identity resolver (cached hashes and publishers), ETW session, supervisor (workers, de-duplication, polling fallback, latency statistics), controller (commands split by domain, tokens, sessions), pipe server, SCM entry point |
| `cli` | Status, setup, recovery, scripted calls |
| `host` | WebView2 window and bridge, window geometry and DPI, notification-area icon, service status monitor, interface preferences, native dialogs |
| `fuzz` | libFuzzer targets for policies, globs, the state parser and the command handler |
| `ui` | Vue application and design system |

Dependencies point downward. `core` knows nothing about Windows.

## Policy semantics

1. A **baseline** of essential system processes and the Houseguest install folder is always allowed and cannot be overridden. It excludes `cmd`, PowerShell, `regedit` and `mmc`, which profiles may restrict.
2. Active rules are ordered by **priority**; at equal priority **Deny wins**. Expired rules are ignored.
3. With no match, the profile **default** applies.
4. Matchers: path glob (case-insensitive, `*` within a segment, `**` across), SHA-256, signer subject (embedded or catalog Authenticode signature chained to a trusted root), package family.
5. `analyze()` reports errors that block activation (malformed matchers, duplicate ids) and warnings that need confirmation (equal-priority conflicts, rules overlapping the baseline, no allowances).

## Authentication and authorization

| Command class | Requirement |
| --- | --- |
| `ping`, `status`, `unlock` | Any local user. `unlock` additionally requires the caller to be the owner account and the correct PIN. |
| `setup`, `recover` | Caller token must be elevated. `setup` works only while no owner exists. |
| Everything else | A token issued by `unlock`, bound to the owner SID, valid 15 minutes, extended on use. |

The pipe has an explicit DACL, rejects remote clients, caps messages at 1 MiB and connections at four per user. The caller identity is read from the pipe's impersonation token, never from the message.

## Persistence

`%ProgramData%\Houseguest` is restricted to SYSTEM and Administrators with a protected DACL. `state.json` carries an HMAC-SHA256 over its content with a key sealed by DPAPI (machine scope). Writes are atomic with a `state.previous.json` snapshot; a file that fails verification is set aside and the previous snapshot is used. `audit.log` is append-only JSON lines where each entry includes the hash of the previous one.

## Session lifecycle

The session (profile, guests, deadline, exceptions) is persisted, so a service restart or reboot resumes it, and a session that expired while the service was stopped ends on the next start with the configured action. Expiry lifts the policy and then, by choice, disconnects or logs off the guest sessions.

## Performance

Measured numbers and how to reproduce them are in [performance.md](performance.md).
