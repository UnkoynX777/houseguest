# Caches, invalidation and interface state

See [metadata.md](metadata.md) for how icons, declared information and signatures are produced and why none of it reaches a decision.

This page lists every cache in Houseguest, who owns it, what makes it stale, and what it is never used for. The rule behind all of it: **a cache may make something faster to show, never easier to allow.** What may run is decided by the policy the supervisor was handed and by facts read from the operating system for that process, not by anything on this page.

## Data flow

| Information | Comes from | Processed in | Stored in | Read by | Sensitive |
| --- | --- | --- | --- | --- | --- |
| Installed programs | Uninstall registry, App Paths, Start menu, packages, Steam, Epic, GOG, Ubisoft, EA, Xbox, winget, Scoop, Chocolatey, PATH, folders registered as portable | catalog providers and the resolver (`system`) | `catalog.json` in the data folder, memory | Applications page, diagnosis | paths only |
| File identity (hash, signer) | the file, read by the service | `IdentityResolver` | memory, keyed by file stamp | supervisor, library, preview | no |
| Policy | owner edits | `compile()` | `state.json` (HMAC), memory | supervisor, library, preview | yes, integrity protected |
| Decisions shown in the library | the policy and the catalog | `apps.library` | memory, keyed by policy revision | Applications page | no |
| History | supervisor and commands | `AuditLog` | `audit.log` (HMAC chain) | History, Dashboard | detail may hold paths |
| Pages | the commands above | `ui/src/cache.ts` | WebView2 memory | pages | no, cleared on lock |
| View preferences | the owner | `ui/src/ui-state.ts` | WebView2 local storage | pages | no |

## Caches

| Cache | Owner | Key | Becomes stale when | Bound | Used for enforcement |
| --- | --- | --- | --- | --- | --- |
| Library catalog | `AppCatalog` | installation id | the change watcher reports a change, the volume set changes, the owner asks, or ten minutes pass | one entry per installation, 4096 | no |
| Executable resolutions | `build()` in `app_catalog.cpp` | install folder, until the executable's size changes | the file changes or the owner picks another program | the catalog | no |
| File identity | `IdentityResolver` | canonical path and `FileStamp` | the stamp changes (size, write time, **change time**, volume and **file id**) | 4096 entries | yes, but only as a memo of facts about the file |
| COM server index | `ComServerIndex` | canonical program path | Windows reports a change below `HKLM\...\CLSID` (`RegNotifyChangeKeyValue`), checked on the next lookup | one set | yes, as evidence for one protected class |
| Compiled profile and library decisions | `Controller` | profile id, then program path and package family | the store revision moves, or the program's size or write time moves | one entry per listed program | **no**, presentation only |
| Icon pictures | `IconService` (host) | every file that could supply the picture, with its change stamp, and the size | any of those files changes | 600 in memory, 45 days on disk | no |
| File details (version resource, signature) | `FileDetails` | canonical path and `FileStamp` | the stamp changes | 512 files | **no**, presentation only |
| Page data | `cache.ts` | page key | the service reports a new policy, catalog or history generation, the session changes, the owner locks, or the age limit passes | 48 slots, least recently used evicted | **no** |

### Why the file stamp and not the write time

The write time of a file can be set from user mode, so a file replaced and given back its old size and write time kept its cached hash and signer. `FileStamp` adds the file system's change time, which user mode cannot set, and the file id, which names the file itself. A test replaces a file's content, restores its size and write time, and checks that the stamp still differs. File times advance with the system clock tick (about 16 ms), so a replacement inside the same tick as the cached read is not distinguishable by time alone; the file id still changes when a file is replaced by rename.

### Enforcement and the policy revision

The supervisor evaluates each process against an immutable `EnforcementPolicy`. Applying a new policy clears the tracked processes and retries, then sweeps, so a process that was allowed under the old policy is judged again under the new one. Each policy carries the store revision it was compiled from, and `diagnostics` reports it as `policy_revision`, so a stale policy is visible. The library's decision cache is separate from this path: it is rebuilt whenever the store revision moves and is never read by the supervisor.

## Invalidation

Events first, timers only as a complement.

| Change | Detected by | Effect |
| --- | --- | --- |
| Program installed or removed | watcher on the Start menu, desktop, program folders, volume roots, packaged apps, Steam manifests and the Uninstall keys | catalog refresh once the burst settles (4 s quiet, 30 s at most); unchanged installations reuse their resolved executable |
| Folder registered as portable | the command itself | the watch set is recomputed at once and the folder is scanned |
| Volume connected or removed | the watch set is recomputed every 30 s; the volume signature is also checked when the library is opened | refresh; entries on a missing volume are kept and marked offline |
| Executable replaced | `FileStamp` | identity and library decision recomputed |
| Profile or rule edited, session started or ended, backup restored | store revision | compiled profile and library decisions rebuilt; the service reports `generations.policy` in every `status` |
| New history event | audit sequence | `generations.history`; History fetches only entries newer than its newest |
| COM registration changed | registry notification | index rebuilt on the next lookup |

The USN change journal was considered and not used. It needs a volume handle with administrator rights per volume, reports every change on the volume, and would only add precision for places the watcher already covers with a handful of directory handles. Recursive monitoring of whole volumes was rejected for the same reason: only the Start menu is watched recursively.

### Concurrency and recovery

* The watcher is one thread. It debounces and calls `AppCatalog::refresh`, which keeps at most one running refresh and one queued follow-up.
* Page slots coalesce concurrent requests for the same key; a response applies only if it belongs to the latest request for that key, so a slow answer never overwrites a newer one.
* A failed refresh keeps the previous data and reports the error; only a page with nothing to show displays the error state.
* The catalog cache carries a version; an unreadable or older file is ignored and rebuilt.
* Nothing from the page cache survives locking: `clearAll()` runs when the interface locks or a command answers `unauthorized`.

## Interface state

Four levels, kept apart on purpose.

| Level | Example | Lives in | Survives |
| --- | --- | --- | --- |
| Component state | an open menu | the component | nothing |
| Navigation state | search text, selection, scroll position, a rule draft | the kept-alive page (`KeepAlive`), scroll positions per page in `App.vue` | moving between pages; reset by locking |
| Preferences | last page, profile, filters, sort, grid or list, history filter | `houseguest.ui` in local storage, version 1 | restarts |
| Domain data | profiles, rules, sessions | the service | everything |

Preferences are validated field by field on read and migrated by version, with the legacy `apps.view` key folded in once; a newer version or damaged data falls back to defaults. The blob holds no token, PIN, search text, selection, scroll position, hash or dialog. Pages close their dialogs when they are left, so a confirmation is never waiting when the owner comes back or restarts.

Placeholders are shown only when a page has nothing to display. They have the size of the content they stand for (cards 188 px, rows 56 px, history rows, profile cards, indicators), the shimmer is slow, and it is switched off under reduced motion. Applications renders a window of the list (the rows or cards on screen plus a margin) inside the page's own scroll container, so the scroll bar still stands for every program. `overflow-anchor` is disabled on the page container because the browser's scroll anchoring fights a windowed list and made the scroll position oscillate.

## Windows components and protection

A restricted account normally runs only the shell and its helpers, and those must not be stopped. The protection table (`core/protection.cpp`) classifies them; it is compiled in and pinned by a test.

| Class | Examples | Evidence required |
| --- | --- | --- |
| Critical process | anything the kernel marks critical or protected | the kernel's flags, read again just before ending a process |
| Sign-in and session, services, desktop shell, input and accessibility | `winlogon`, `services`, `svchost`, `explorer`, `ctfmon` | exact location under the Windows folder |
| Shared components | `dllhost`, `conhost` (System32 and SysWOW64) | exact location |
| System infrastructure | error reporting, search, printing, update orchestration, `CompPkgSrv` | exact location **and** the Windows signing identity (`CN=Microsoft Windows`), not any Microsoft certificate |
| Driver and system apps | `DriverStore\FileRepository`, `SystemApps` | location and a valid signature |
| COM servers | `mobsync`, `BdeUISrv`, `UserOOBEBroker` | registered in `HKLM` as a COM local server, Windows signed, started by the DCOM launcher, not a tool that runs arbitrary code |
| Kernel drivers | `.sys` files | never processes; the service does not evaluate them |

Everything else under the Windows folder is an ordinary program decided by the profile. In particular a program that only sits in System32 is not protected: on the machine this was tested on, `ReduceMemory_x64.exe` (third party) and `wallpaperservice64.exe` (third party) live in System32 and stay under the profile, while `CompPkgSrv.exe`, `UserOOBEBroker.exe`, `mobsync.exe` and `BdeUISrv.exe`, all signed `CN=Microsoft Windows`, are now recognised. Administrative tools (`cmd`, `powershell`, `rundll32`, `msiexec`, `mmc` and the rest of the sensitive list) are never granted by evidence, whatever starts them.

Limits, stated plainly:

* Infrastructure that is neither in the table nor a registered COM server (for example something only the Task Scheduler starts) is judged by the profile. The block is recorded with `windows_component: true` and History says so, so the owner can allow it.
* The evidence rule trusts that an administrator wrote the COM registration and that the DCOM launcher is `svchost.exe` in System32; a guest cannot write either, but an administrator can.
* A process started in a different way than it was last time is judged again; nothing is inherited from its parent by name.

## Measurements

Taken on the development machine (Windows 10, a local installation with 339 catalogued entries), with a private service instance, by `scripts/measure-library.ps1`. Times are wall-clock inside the service unless stated.

| Measure | Before | After |
| --- | --- | --- |
| Full library scan, cold | 17.8 s | 2.4 s |
| Refresh when nothing changed | 23.4 s | 2.4 s, and only when something changed or ten minutes passed |
| `apps.library`, steady state | 215 ms | 8.2 ms (40 calls) |
| `apps.library`, including the first call after a policy change | 215 ms | 31 ms average, 277 ms for the call that rebuilds the cache |
| Library decisions reused | n/a | 88 % over the measured run |
| Library payload | 258 KB | 227 KB (evidence and candidates only for entries that need a choice) |
| Time to the first catalog after a restart | 0.88 s | 0.65 s |
| Service memory after a scan | 14 to 20 MB working set across runs | 11 MB working set, 14 MB private; the run-to-run spread is as large as the difference, so no change is claimed |

Where the time went: the scan spent about 19 of its 20 seconds in the progress preview and the merge step, which resolved the canonical form of every path against every entry. Paths and file checks are now looked up once per build and previews are limited to one per 750 ms. The library request spent 150 ms of 215 ms re-reading file metadata for every program; decisions are now reused for as long as the policy and the file are unchanged.

The interface figures come from `scripts/ui-check/run.ps1`, a scripted browser session against a mock service, so they describe the page logic and not the service: first visit asks once and shows a placeholder, returning asks nothing and shows none, a policy generation change refreshes the page without a placeholder, and 1500 programs render about 35 cards or 24 rows.

Not measured: CPU of the service during a scan beyond the observation that the scan runs on one thread at below-normal priority plus up to three resolvers; the first-paint time inside the real WebView2 window; behaviour with a library of several thousand entries on slow disks.
