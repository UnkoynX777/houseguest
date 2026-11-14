# Performance

Measured with `scripts\measure.ps1` on one machine: Windows 10 Pro 22H2, Release build, x64, 60-second samples. These numbers describe that machine, not a guarantee.

| Metric | Result |
| --- | --- |
| Service memory, no session | 3.4 MB private bytes (9.6 MB working set) |
| Service memory, active session | 3.8 MB private bytes |
| Service CPU, no session | 0.017 % |
| Service CPU, active session | 0.02 % |
| Interface host window visible | about 60 ms after launch (the web content loads afterwards) |
| WebView2 processes started by the interface | about 280 MB in total |

The interface is not needed for protection. Closing it (or exiting from the tray) frees the WebView2 processes and leaves the service at a few megabytes.

## Time from process-start event to termination

`scripts\e2e.ps1` launches twelve denied programs at once under a guest account and reports the delay between the kernel's process-start event and the termination, measured by the service itself:

> average 1.6 ms, 95th percentile 1.6 ms, maximum 1.6 ms over 12 samples

This is the delay *after* Windows reports the start. It does not include the time the program ran before the event, and Houseguest cannot stop a program that finishes before the event is handled. A signature or hash rule can add the time needed to compute it on the first launch of a large file; results are cached afterwards. The same figures are shown in **Diagnostics → Monitoring performance** on your machine.

## Fallback mode

When Event Tracing is unavailable the service checks running programs once per second. CPU stays low, and blocking can be about a second slower.
