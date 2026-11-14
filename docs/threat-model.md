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
| Audit tampering | Hash chain | Audit tests |
| Accidental lockout | Baseline, analysis, preview, audit mode, timed sessions | Core tests |
| Malformed input | Strict JSON parsing with limits | Serialization tests |

## Not yet verified

Fuzzing of the pipe protocol, sanitizer builds, behavior under Windows 11 and non-Pro editions, and an independent security review. Until then, treat guarantees as design intent backed only by the tests listed above.

## Residual risks (accepted and disclosed)

Reactive enforcement window; owner-account guests rely on PIN strength; per-user connection cap can be filled by that user (denial of service of the pipe for that user only).
