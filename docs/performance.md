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

## Decision cost

Measured by the service while it assessed every process of a busy desktop session and a launched application tree (`scripts/e2e-continuity.ps1`, Debug build, audit mode): assessing one process, which covers the kernel flags, token, image, parent lookup and the engine, took 4 to 12 microseconds on average and 10 to 27 microseconds at the 95th percentile once the file identity was cached. Hash and signature checks are cached by path, size and modification time, so they cost time only on the first launch of a file; a signature check of a catalog-signed program can take about a second the first time, which is why observation lists a program as soon as it is hashed and fills in the signer afterwards. Decisions are not cached: they are cheap and depend on the lineage of the moment. Queue depth and high-water marks are reported, and a full lineage table is counted and shown in Diagnostics.

## Fallback mode

When Event Tracing is unavailable the service checks running programs once per second. CPU stays low, and blocking can be about a second slower.

Library scan, request latency, cache hit rate and the service memory with a populated catalog are measured by `scripts\measure-library.ps1`; the results and the before/after comparison are in [caching.md](caching.md#measurements).
