# Frequently asked questions

## What is Houseguest?

An open-source Windows app blocker for guest sessions. It lets the owner of a PC decide which programs a guest account may run for a set time. A Windows service enforces the rules independently of the interface.

## How is it different from Windows parental controls or Group Policy?

Microsoft Family Safety needs Microsoft accounts and online management; Group Policy and AppLocker are aimed at managed environments and do not behave the same on every edition. Houseguest is local, works the same on Home and Pro, needs no account, and is built around a short guest session with a timer. It does not replace organisational policy tools.

## Does it block programs before they start?

No. It terminates a program shortly after Windows reports that it started. A blocked program can run for a moment, and a very short-lived program can finish before it is stopped. The documentation and the interface say this plainly. See [architecture](architecture.md) and [performance](performance.md).

## Can the guest turn it off?

A standard user cannot stop the service, edit its data, read the audit log, claim ownership or uninstall it (tested). A local administrator can always remove it, by design: it is not malware and does not hide. Give your guest a standard account, not an administrator account.

## Can I lock myself out?

Essential Windows processes and Houseguest itself cannot be blocked, sessions always end, and an administrator can run `houseguest-cli recover`. See [troubleshooting](troubleshooting.md).

## Does it work on Windows 11, Home, Enterprise, or modified builds like Ghost Spectre?

Only Windows 10 Pro 22H2 has been tested. Houseguest probes the machine and degrades when something is missing; see [compatibility](compatibility.md). Reports from other systems are welcome.

## Does it use AppLocker, App Control (WDAC) or Windows Defender?

No, and it will not. That keeps the behaviour identical across editions and avoids machine-wide policy changes.

## Does it collect data or phone home?

No telemetry. See [privacy](privacy.md).

## Why is the installer unsigned?

Code signing certificates cost money and the project does not have one yet. Verify downloads with `SHA256SUMS.txt` and the GitHub provenance attestation.

## Who can I trust to review it?

Everyone: the source, the build workflow and the release provenance are public. Pull requests are reviewed by a human. Security issues go through [SECURITY.md](../SECURITY.md).
