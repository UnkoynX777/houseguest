# Roadmap

Status: **done** = implemented and verified as described; **planned** = not started.

## 0.1.0 (done)

Policy engine, supervised enforcement, guest sessions with timer, audit mode and exceptions, signed store, audit chain, authenticated pipe, interface (ten screens, en/pt-BR, light/dark), CLI, MSI and portable zip. Verified on Windows 10 Pro 22H2: unit tests, `e2e.ps1`, `install-test.ps1`, scripted interface run.

## Next

| Item | Why it matters |
| --- | --- |
| Windows 11 and other editions test matrix | Only Windows 10 Pro has been exercised |
| Fuzzing the pipe protocol and the store parser; ASan builds in CI | Memory-safety assurance for the privileged service |
| App Control for Business backend | Preventive, kernel-enforced rules (machine-wide) where supported |
| AppLocker backend | Per-user preventive rules on Enterprise/Education |
| Microsoft Store (packaged) app rules by package family | Today packaged apps are matched by executable path or publisher only |
| Code signing for installer and binaries | Removes SmartScreen friction and proves origin |
| App icons in the interface | Currently initials tiles |
| Performance measurements | Publish idle CPU, memory and start-up numbers |
| Accessibility audit with a screen reader | Keyboard and ARIA basics exist; not audited |
| Update notifications with signature verification | Today the About page only checks the GitHub API on request |

## Open decisions

- Name clearance (trademark search) for "Houseguest".
- Code-signing certificate source.
