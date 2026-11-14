# Troubleshooting

Open **Diagnostics** first. It lists every dependency Houseguest checks, with a copyable report.

## The interface says the service is not running

Open Services (`services.msc`), find **Houseguest Access Control** and start it. From an elevated prompt: `net start Houseguest`. The service restarts itself after a crash. If it never starts, check the Windows Event Viewer and the installer log.

## The interface does not open and offers a download

The Microsoft Edge WebView2 Runtime is missing. Accept the prompt to open Microsoft's official download page, install the Evergreen runtime and start Houseguest again. Protection and `houseguest-cli` keep working without it.

## "Reduced monitoring mode" on the dashboard

Event Tracing for Windows could not start a session, so Houseguest checks running programs every second instead. Blocking can be about a second slower. Diagnostics shows the Windows error code. Houseguest retries every 15 seconds and switches back on its own.

## A session will not start

- *A component Houseguest depends on is missing* — a critical check failed. Open Diagnostics.
- *Process supervision is not running* — start the session in audit mode or fix the cause shown in Diagnostics.
- *Review the warnings* — the policy has conflicts or blocks nothing useful; read the findings and confirm.

## The tray icon is not visible

Windows may place new icons in the overflow area. Drag the icon onto the taskbar, or use **Settings → Personalization → Taskbar → Other system tray icons**. If Explorer restarts, Houseguest re-adds the icon by itself. Closing the window hides it to the tray when that option is on (Settings), and **Exit** in the tray menu closes the interface without stopping protection.

## I forgot the owner PIN or I am locked out

From an elevated prompt:

```
houseguest-cli recover               # ends the active session
houseguest-cli recover --reset-owner # also clears the owner and PIN
```

## Uninstalling

Remove Houseguest from Installed apps (administrator rights required). While a guest session is active the uninstaller stops with an error so a session is not removed by accident. End the session first, or run an administrator command prompt:

```
msiexec /x Houseguest-<version>-x64.msi FORCE_UNINSTALL=1
```

Settings and the audit log remain in `%ProgramData%\Houseguest` until you delete that folder.

## A legitimate program is blocked

Open **History**, find the entry and add a temporary exception, or add an allow rule in the profile. The **Preview** in Sessions shows which installed applications a profile would block before you start.

## Reporting a problem

Open an issue with the Diagnostics report. Do not post your PIN or audit log. Report vulnerabilities privately as described in [SECURITY.md](../SECURITY.md).
