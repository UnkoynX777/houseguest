# Compatibility

Houseguest uses documented Windows APIs only: Event Tracing for Windows, named pipes, DPAPI, CNG, WinVerifyTrust, the Service Control Manager and the Windows Terminal Services session API. It installs no kernel driver and does not depend on Windows Defender, SmartScreen, AppLocker or App Control.

## What has been verified

| Platform | Status |
| --- | --- |
| Windows 10 Pro 22H2 (build 19045), x64 | Unit tests, enforcement, installer, permissions, uninstall guard, window and tray checks, interface run |
| Windows 11, any edition | **Not tested** |
| Windows 10 Home, Enterprise, Education, LTSC | **Not tested** |
| Modified or reduced builds (for example Ghost Spectre) | **Not tested.** See below |
| ARM64 | Not built |

Nothing here is claimed as supported until it appears in the first row group above.

## Runtime capability probe

Instead of assuming a stock Windows, the service probes what is really available when it starts and again whenever you open **Diagnostics**:

| Check | If it fails |
| --- | --- |
| Windows build (19041 or later recommended, 10240 minimum) | Older than recommended: warning. Older than the minimum: sessions cannot start. |
| Cryptography and DPAPI machine key | Sessions cannot start, because settings could not be stored safely |
| Process inspection | Sessions cannot start |
| Process creation events (ETW) | **Degraded mode**: running programs are checked every second. A blocked program may run about a second longer. The dashboard says so |
| Publisher verification (WinVerifyTrust, Cryptographic Services) | Publisher rules are unavailable; path and hash rules keep working |
| Session control (WTS) | Guests cannot be disconnected or signed out when time is up |
| Local accounts | Reported |
| WebView2 Runtime | The interface cannot open; Houseguest offers the official download page. Protection and the command line are unaffected |

A session never starts if a critical check is missing or if process supervision is not running (audit mode excepted), so the product does not pretend to protect when it cannot.

## Modified and reduced Windows builds

Some community builds remove services, components or security features. Houseguest does not detect or judge such builds by name. It reports what the probe finds and degrades as above. Two honest consequences:

- A build that removes Event Tracing, the Cryptographic Services service, the Terminal Services session API or the WebView2 Runtime loses the matching feature, and Houseguest says which.
- A build that weakens the standard security model (for example by allowing standard users to control services or write to `%ProgramData%`) weakens Houseguest in the same way. Houseguest relies on Windows access control for its own protection.

If you run one of these builds, open **Diagnostics**, copy the report and attach it to a [compatibility report](https://github.com/UnkoynX777/houseguest/issues/new?template=bug_report.yml). Verified results will be added to the table above.

## Optional components

| Component | Needed for | Offered when missing |
| --- | --- | --- |
| Microsoft Edge WebView2 Runtime | The interface | Yes, a prompt links to Microsoft's official download page |
| Cryptographic Services (`CryptSvc`) | Publisher rules for catalog-signed Windows files | No, reported in Diagnostics |
| Windows Defender | Nothing | Not used |
