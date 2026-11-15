# Threat model

The source code is public. Security rests on Windows access control, authenticated IPC and cryptography, never on secrecy of the implementation.

## Assets

1. The policy and its integrity.
2. The owner's authority to start, stop and change sessions.
3. Availability of the computer to its owner (no permanent lockout).
4. The audit log.
5. The owner PIN verifier.

## Actors and guarantees

| Actor | Can do | Houseguest guarantees | Houseguest does not guarantee |
| --- | --- | --- | --- |
| Standard user, window closed | Close the interface, try to kill or reconfigure the service, edit files they can write, rename executables, guess PINs | Enforcement continues. Service control and the data folder are denied by ACLs (tested). Hash and publisher rules ignore renaming. PIN attempts are rate limited with persistent back-off. | Blocking programs during the brief window before termination. |
| Guest during a session | Same, plus running allowed programs | The session ends at its deadline. Allowed programs cannot send privileged commands. | Preventing abuse inside allowed programs, or launching other programs from them without a rule. |
| Guest using the owner's own account | Can reach the unlock prompt | Only PIN guessing is possible, with back-off. | A short PIN. Use a long PIN, or a separate guest account. |
| Local administrator | Anything | Tampering with state or audit log is detectable. Recovery and removal always work. | Prevention. An administrator owns the machine. |
| Malware as the user | Talks to the pipe, reads user-writable files | Commands need the owner token, which needs the PIN and the owner SID. The service data is not readable. | Defense against admin-level or kernel-level malware. |
| Physical access | Boot media, disk removal | Documentation recommends BitLocker and a firmware password. | Any protection. |

## Principles

- **No stealth.** No process hiding, no hidden persistence, no anti-debugging, no evasion of security tools. The service is listed in Services with a clear description.
- **Always recoverable.** An elevated administrator can end a session or reset the owner with `houseguest-cli recover`, and can uninstall. Safe Mode uninstall works.
- **Fail safe for the owner.** The protected Windows components and Houseguest itself cannot be denied, Houseguest cannot be blocked from the guest's own session, policies with errors cannot be activated, and sessions always have an end.
- **Least privilege.** The interface runs unelevated. Only the service is privileged, and it exposes a small, validated command set.

## Control mapping

| Threat | Control | Verified by |
| --- | --- | --- |
| Service stopped by standard user | Default service DACL grants control to SYSTEM and Administrators only | `install-test.ps1` |
| Reading or editing state | Protected DACL on the data folder, HMAC over the state, atomic writes | `install-test.ps1`, store tests |
| Claiming ownership after install | `setup` requires an elevated token and an unconfigured service | `install-test.ps1` |
| Pipe spoofing | `FILE_FLAG_FIRST_PIPE_INSTANCE`, explicit DACL, no remote clients | Code review |
| Unauthorized commands | Per-command access class, owner token bound to SID | `e2e.ps1`, `install-test.ps1` |
| PIN brute force | PBKDF2-SHA256 (600,000 iterations), persisted exponential back-off | Code review |
| Audit tampering | Keyed HMAC chain, keyed anchor against truncation, rotation, torn-record quarantine. An administrator can still read the key | Audit tests, `e2e-security.ps1`, libFuzzer target |
| Pipe exhaustion by idle or slow clients | Deadlines on every read and write, pending and per-user limits, rate limit, size and nesting limits | `e2e-security.ps1` (40 idle clients, oversized, nested and truncated messages, request bursts) |
| Guest account left usable after a session | Two-phase end that retries until the expiry action works, managed accounts disabled with a Windows-enforced expiry | `e2e-security.ps1` (restart during a session, expiry while the service is down) |
| Service tricked into touching a network path | Network image paths and shortcuts are never opened by the service | System tests, code review |
| Transient inspection failure treated as permission | Explicit per-process states, bounded retries, failure counters and audit events | Code review, Diagnostics counters |
| Weak service process | Unused privileges removed at startup, safe DLL search path, no remote or low-integrity image loads, extension points disabled | `e2e.ps1` runs with the reduced token |
| Interface as an attack path | Navigation, frames, new windows and permission requests denied; no host objects, dialogs or autofill; CSP; every privileged command re-validated by the service | Code review |
| Uninstall during a guest session | The MSI asks the service whether a session is active and aborts unless an administrator passes `FORCE_UNINSTALL=1`; removal needs elevation either way | `install-test.ps1` |
| Event session stopped or unavailable | Fallback to polling every second, retry of the event session, audit entry on every mode change, no enforcing session starts without supervision | `e2e.ps1` (stops the event session, checks polling, enforcement and recovery) |
| Accidental lockout | Protection table, continuity, analysis, preview, audit mode, timed sessions | Core tests |
| Malformed input | Strict JSON parsing with limits | Serialization tests, libFuzzer targets for policies, globs, state and the command handler (AddressSanitizer) |
| Recycled process ids | Single process handle per inspection, creation-time check against the event, parent snapshots newer than the process, lineage records that hold the process handle | Code review, `e2e.ps1`, `e2e-continuity.ps1` |
| Houseguest blocked by its own profile | Kernel-critical and protected processes, a reviewed Windows table, Houseguest files recognized by pinned hash from an unwritable folder, WebView2 recognized by Microsoft signature, runtime folder and a verified Houseguest parent | `e2e-continuity.ps1` (the host and six WebView2 processes stay allowed, also after a service restart) |
| Allowed application used to run a blocked tool | Dependencies pinned by hash or signer and scoped below their application; unlisted programs blocked; tools that run programs never inherited; a renamed or copied tool matches no pin; a signed Windows tool copied elsewhere is no longer Windows | Core tests, `e2e-continuity.ps1` |
| A pinned dependency replaced after approval | A hash mismatch is reported as changed and blocked until the owner approves the new file, or the file follows an update from the pinned signer | Core tests |
| Critical Windows process ended by mistake | Kernel flags are read again immediately before termination; uncertain or unreadable critical processes are spared and logged | Code review, Diagnostics counter |
| Dependency approved from a stale view | Approvals re-read the file and fail if its hash differs from the one shown | Code review |

## Not yet verified

Behaviour on Windows 11 and non-Pro editions, behaviour on modified or reduced Windows builds, and an independent security review. Fuzzing so far consists of short local runs (about 150 seconds per target, no findings) and a 30-second smoke run in CI; it is not a substitute for a long campaign. Treat guarantees as design intent backed by the tests listed above.

## Residual risks (accepted and disclosed)

- Reactive enforcement window.
- Owner-account guests rely on PIN strength.
- A user can fill their own connection allowance, which only denies service to that user's own clients for a few seconds.
- Rules match the user in the process token, not the logon session. A program the guest starts with other credentials (for example `runas` with an administrator password the guest knows) is outside the policy.
- Session deadlines use the system clock. Changing the clock requires administrator rights.
- The audit key is readable by administrators, so an administrator can forge a consistent log.
- Files that change between inspection and execution are re-checked once after hashing; a program replaced by a different file at the same path after inspection is not detected.
- Rules for network programs can only match their path.
- The parent a process reports can be set by a process that may open its intended parent. Lineage therefore only narrows what a pinned component may do; it never grants anything by itself, except for unlisted programs in related mode, which also need the application's signer (or its folder for unsigned applications).
- A dependency that starts and finishes before the event is handled is not evaluated, as with any reactive enforcement. Evaluation waits briefly for the parent's membership so a dependency is not judged before its application is known, and gives up after about twelve seconds.
- Related mode on an unsigned application trusts the install folder. If guests can write there, pin the components instead.
- The WebView2 check does not read command lines. A process that may open the Houseguest host as its parent could start a Microsoft-signed WebView2 runtime executable and have it recognized; it is a browser engine without a host window.
- Protected directory grants (driver packages and system apps) trust the signature and the administrator-only location of those folders.

## Remote access and the assistant (v5)

New actors appear only when those optional features are configured.

| Actor | Can do | Houseguest guarantees | Houseguest does not guarantee |
| --- | --- | --- | --- |
| Anyone on the internet | Request the public address | Requests are refused before the API unless they carry a Cloudflare Access token that verifies (RS256 signature, issuer, audience, expiry), and they still need a device key. With remote access off, every request that looks proxied is refused. The setup refuses to create an Access application without an allowed e-mail, and **Check that it is protected** reports a failure if the API answers anonymously. | Protection if the Cloudflare account itself is compromised, or if the Access policy is later widened in the Cloudflare dashboard. |
| Someone on the local network | Connect to the computer | The API listens on `127.0.0.1` only, and the port is held exclusively. | Nothing beyond that. |
| Another local program | Connect to the loopback port, guess keys | Every route needs a 256-bit device key (only its hash is stored); failed attempts are rate limited; Host and Origin must match; mutating requests need a fresh timestamp and a unique nonce. | A program running as the owner or an administrator can read what the owner can read. |
| Holder of a stolen read key | Read state until it is removed | Cannot change anything. Keys expire after 30 days or 12 hours of disuse and can be removed at once. | Confidentiality of what that key could read. |
| Holder of a stolen write key | Request changes | Sensitive changes also need the owner PIN. Wrong PINs lock remote confirmation with growing delays, on a counter separate from the local PIN. The API cannot reach settings, PIN, accounts, backups, recovery, remote setup or the assistant. A remote session start cannot be audit-only. | Protection against someone who also knows the PIN. |
| Cloudflare | Terminate TLS, see requests | Traffic is encrypted in transit on both legs. | Confidentiality from Cloudflare. No end-to-end layer is added: it would not remove the need to trust the tunnel keys, and a paired-key design would be a separate project. |
| Text from the computer (names, paths, logs) | Contain instructions aimed at the assistant | Treated as data; control characters removed; the model has read tools and three proposal tools only; proposals are validated, simulated and applied only after the owner presses Apply; no tool disables protection, edits settings or writes rules. | That a model never follows injected text. The design limits what following it can achieve. |
| Google | Receive the content of questions | Nothing is sent while the assistant is off; the key is DPAPI-protected; the key travels only in a header. | Privacy on the free tier, where Google may use the content. |

Residual risks specific to v5: an administrator can read the device registry and the DPAPI-protected secrets of the machine; the Cloudflare API token is held in memory for the duration of the setup; `cloudflared` is trusted after an Authenticode check that names Cloudflare and is not pinned to a version; remote PIN attempts are limited per computer, so an attacker with a write key can lock **remote** confirmation (never the local PIN) for a while.
