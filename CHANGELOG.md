# Changelog

All notable changes are documented here. The project follows [Semantic Versioning](https://semver.org/) and the format of [Keep a Changelog](https://keepachangelog.com/).

## [Unreleased]

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

### Known limitations
- Enforcement is reactive. AppLocker and App Control backends are not implemented yet.
- Packaged (Microsoft Store) apps are matched by executable path or publisher only.
- Builds are not code signed.
- Validated on Windows 10 Pro 22H2 only. Windows 11 and other editions are untested.
