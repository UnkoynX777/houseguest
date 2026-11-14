# Feasibility and Windows limitations

Houseguest's promise is bounded by what Windows can actually enforce. This document states what the mechanism guarantees and what it cannot do.

## Decision: own supervision only

Houseguest enforces policy by observing process creation and terminating processes that the rules do not allow. It does not use AppLocker, App Control for Business (WDAC) or Explorer policies. Those mechanisms differ by edition, are machine-wide or need Group Policy, can require reboots and are unreliable on reduced or modified Windows builds. Using one mechanism everywhere keeps behaviour predictable and honest.

## How supervision works

| Mode | Mechanism | When | Reaction |
| --- | --- | --- | --- |
| Events | Kernel process-start events from Event Tracing for Windows, a safety sweep every 5 seconds | Normal | Stops a denied process as soon as the event is delivered |
| Polling | Running processes checked every second | Event Tracing unavailable or its session ended; Houseguest retries every 15 seconds | Up to about a second later; the dashboard says so |
| Stopped | Nothing | Supervision cannot run | Sessions that enforce cannot start; audit mode still can |

For each process the service opens a handle once, reads the owner, image path and creation time from that handle and terminates through the same handle, so a recycled process id can never be mistaken for another process. A process whose id was reused after the event is skipped and gets its own event.

## What cannot be guaranteed

- **Prevention.** Termination follows launch. A program that finishes before the event is handled cannot be stopped, and a blocked program can run for a short moment.
- A local administrator can remove any of this. The design preserves that on purpose (see `threat-model.md`).
- Physical access allows booting another OS or resetting the machine. BitLocker and firmware passwords are the correct controls.
- Allowed programs can launch other programs. A browser can run a web-based tool. Launchers need explicit rules; the preview shows them.
- Renaming defeats path rules. Hash and publisher rules do not suffer from this, and the rule editor steers users toward them.
- Updates change hashes. Publisher rules are recommended for frequently updated software.
- The supervisor does not use a kernel driver, so it cannot suspend a process before its first instruction runs.

## Capability detection

The service probes Windows build, cryptography, process access, event tracing, signature verification, session control, local accounts and the WebView2 Runtime, and reports each in Diagnostics. See [compatibility](compatibility.md). Detection is done by probing, not by edition name.

## Development note

Development and verification so far ran on Windows 10 Pro (19045). Windows 11 and other editions have not been tested.
