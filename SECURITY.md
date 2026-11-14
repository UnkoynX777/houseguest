# Security policy

## Reporting a vulnerability

Please report privately through GitHub: **Security → Report a vulnerability** on this repository
(<https://github.com/UnkoynX777/houseguest/security/advisories/new>). Do not open a public issue.

Include the Houseguest version, Windows build, what an attacker needs (standard user, guest, administrator, physical access) and steps to reproduce. You will get an acknowledgement as soon as the maintainer sees the report. This is a volunteer project, so no response time is guaranteed, and we ask for reasonable time to fix a problem before you disclose it.

## What counts as a vulnerability

Houseguest documents which attacker it defends against in [docs/threat-model.md](docs/threat-model.md). In scope:

- A standard user or guest stopping, bypassing or reconfiguring protection while the session is active, or reading or altering the service data.
- Claiming ownership, unlocking or sending privileged commands without the required authorization.
- Memory-safety or parsing flaws in the service, the pipe protocol or the store.
- Flaws in the audit chain, state integrity check or PIN handling.

Out of scope, by design: local administrators, kernel-level malware, physical access, and the documented reactive window of process supervision.

## Supported versions

Only the latest release receives fixes while the project is pre-1.0.
