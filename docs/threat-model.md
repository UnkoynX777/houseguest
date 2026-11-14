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
- **Fail safe for the owner.** The baseline of essential processes cannot be denied, Houseguest cannot be blocked from the guest's own session, policies with errors cannot be activated, and sessions always have an end.
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
| Accidental lockout | Baseline, analysis, preview, audit mode, timed sessions | Core tests |
| Malformed input | Strict JSON parsing with limits | Serialization tests, libFuzzer targets for policies, globs, state and the command handler (AddressSanitizer) |
| Recycled process ids | Single process handle per inspection, creation-time check against the event | Code review, `e2e.ps1` |

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
