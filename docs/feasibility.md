# Feasibility and Windows limitations

Houseguest's promise is bounded by what Windows can actually enforce. This document
states what each mechanism guarantees, on which editions, and what it cannot do.
Claims here must be re-verified against Microsoft documentation before a release
makes them in user-facing text.

## Status in 0.1.0

Only **tier 3 (supervised mode)** is implemented. Tiers 1, 2 and 4 are documented research, not features: Houseguest does not use App Control, AppLocker or Explorer policies yet, and the interface says so. The edition and capability detection reports what the machine could support.

## Enforcement tiers

Houseguest reports the enforcement tier it can honestly provide on the current machine.
The UI never shows "Protected" for a tier that cannot deliver it.

| Tier | Mechanism | Scope | Strength | Editions (to verify per release) |
|------|-----------|-------|----------|----------------------------------|
| 1 | App Control for Business (WDAC) policy | Machine-wide | Kernel-enforced, hash/publisher/path/package rules | Policy can be applied on all editions; Group Policy deployment is Enterprise/Education. First policy activation requires a reboot. |
| 2 | AppLocker | Per user or group SID | Enforced by the Application Identity service | Enforcement supported on Enterprise/Education/Server; not supported on Home and, officially, Pro. |
| 3 | Supervised mode | Per user (via process token) | Reactive: process creation is observed through ETW and the process is terminated | All editions. A race exists between launch and termination; a short-lived process can run. |
| 4 | Explorer policies (`DisallowRun` / `RestrictRun`) | Per user (HKCU) | Shell launches only | All editions. Does not stop programs started by other programs. |

Consequences that shape the product:

- **Tier 1 is machine-wide.** It cannot distinguish the owner from the guest. Houseguest
  therefore pairs it with a guest-session model: the policy is active while a guest
  session is armed, and the owner's tools are covered by an explicit owner allowance
  (signed by the owner's publisher/hash set), not by identity.
- **Tier 2 gives true per-user rules** but only where Windows enforces them.
  Capability detection decides at runtime; Houseguest does not assume.
- **Tier 3 is honest best effort.** It is the only universal fallback and is labelled
  "Soft" in the UI and in exports.
- Tiers combine. The effective tier is the strongest one that is available, and the
  Security screen lists what each active tier does not cover.

## What cannot be guaranteed

- A local administrator can remove any of this. The design preserves that on purpose
  (see `threat-model.md`).
- Physical access allows booting another OS or resetting the machine. BitLocker and
  firmware passwords are the correct controls; Houseguest recommends them but does not replace them.
- Allowed programs can launch other programs. A browser can run a web-based tool.
  Launchers and child processes need explicit rules; the policy preview shows them.
- Renaming defeats path rules. Hash, publisher and package-family rules do not suffer
  from this, and the rule editor steers users toward them.
- Updates change hashes. Publisher/version-range rules are recommended for
  frequently updated software; hash drift is detected and reported.

## Edition and capability detection

At start-up the service computes a `Capabilities` record (edition, build, whether the
Application Identity service can run, whether CI policy deployment succeeds in audit
mode, WebView2 runtime presence). Features missing from it are hidden or disabled with
an explanation. Detection is done by probing, not by edition name alone.

## Development note

Development and all verification so far ran on Windows 10 Pro (19045). Windows 11 is the primary target and has not been tested.
