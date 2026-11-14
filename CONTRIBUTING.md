# Contributing

Thank you for considering a contribution. Houseguest touches system security, so changes get careful human review. Nothing is merged automatically.

## Before you start

- Open an issue or discussion for anything larger than a small fix, so we can agree on the approach first.
- Security problems go through [SECURITY.md](SECURITY.md), never a public issue.
- Houseguest uses official Windows mechanisms only. Proposals involving process hiding, undocumented APIs, evasion of security tools or hidden persistence will be declined.

## What we expect from a pull request

The template asks for the motivation, the technical approach, risks, how you tested it, and a justification for any new dependency. Contributions should be code you wrote or fully understand and can defend in review. Tools that help you are fine; being unable to explain your change is not.

Approval depends on the quality of the change: correctness, clarity, tests that exercise the behavior, and honesty about what was and was not verified.

## Development

```powershell
cd ui; npm ci; cd ..
.\scripts\build.ps1 -RunTests          # Debug build and unit tests
```

While iterating, run only what you touched, for example `ctest --test-dir build\debug -R core`. The full suite and the elevated scripts (`e2e.ps1`, `install-test.ps1`) are for milestones and before releases.

## Code standards

- C++23, formatted with `clang-format` (the repository `.clang-format`), checked with `clang-tidy` (`.clang-tidy`). Warnings are errors.
- `core` must not include Windows headers. `scripts/check-repo-hygiene.ps1` enforces this and also rejects private tool files and credentials.
- Optional local hooks: `git config core.hooksPath scripts/git-hooks` runs the hygiene check before each commit and `scripts/check-commit-message.ps1` on each message. CI runs both on every push, and rejects automatic tool attributions in commit trailers.
- Prefer clear names and small functions over comments. Comment only invariants and security decisions.
- No new dependency without a reason that the standard library or Windows cannot satisfy. Pin by version or commit.
- UI strings are English keys with Portuguese translations in `ui/src/pt-BR.ts`.
- Behavior or guarantee changes update `docs/threat-model.md` and the README limitations.

## Good first contributions

Look for the `good first issue` label: translations, documentation fixes, new built-in profile ideas, UI polish and tests.
