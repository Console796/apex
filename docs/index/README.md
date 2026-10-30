# Apex repository index

Start here if you have not seen this repository before. Apex is a native, cross-platform level
editor for Source-engine maps (`.vmf`), aimed at the Strata Source / P2CE toolchain. It is written
in C++23, uses Qt 6 for the shell and Vulkan for the viewports. The product description and the
list of what works today are in the [root README](../../README.md).

These index pages say where things are and how the code fits together. They do not replace the
design documents in [docs/](../); they point at them.

## Reading order

1. **This page.** Which document answers which question, and the conventions used everywhere.
2. [repo-map.md](repo-map.md): what each top-level directory and file is for.
3. [architecture.md](architecture.md): the layer rules, the core model, and how a user action
   reaches the screen.
4. [source-map.md](source-map.md): the directories under `src/`, file by file for the important
   ones.
5. [find-by-task.md](find-by-task.md): "I need to change X, where do I start?"
6. [tests-tools-docs.md](tests-tools-docs.md): how tests are organised, the CI gate, the helper
   scripts, and the design documents.
7. [glossary.md](glossary.md): Source, Strata and project terms.

## Which document answers which question

| Question | Go to |
|---|---|
| What is Apex and what works today? | [../../README.md](../../README.md) |
| What version is this, and which milestone is next? | [../../version.txt](../../version.txt), then [../roadmap.md](../roadmap.md) |
| How do I build, test and run it? | [../development.md](../development.md) (procedure), [../../CONTRIBUTING.md](../../CONTRIBUTING.md) (rules) |
| Why was it designed this way? | [../research.md](../research.md) (background and options), [../plans/](../plans/) (per-milestone decisions) |
| What did we decide about one specific thing? | Search the plans for its decision number, for example `grep -rn "D13" docs/plans` |
| How does Strata/Hammer behave, and what did we verify? | [../plans/addons.md](../plans/addons.md), [../plans/probe-results/README.md](../plans/probe-results/README.md) |
| Which Source games has Apex been tested against? | [../source-game-support.md](../source-game-support.md) (only P2CE is tested) |
| Which third-party code is used and under what licence? | [../../THIRD_PARTY.md](../../THIRD_PARTY.md) |

## Conventions you will see everywhere

- **Decision numbers.** Plans number their decisions `D1`, `D2`, and so on. Code comments and
  commit messages cite them, for example "plan 0.1, D13" for the renderer context. A decision is
  the reason behind a piece of code, so when a comment cites one, read it.
- **Milestones and sub-milestones.** The roadmap is split into milestones (0.1 to 0.10, then 1.0).
  Each has a plan in `docs/plans/`. Sub-milestones are numbered `0.3.1`, `0.4.6`, and so on.
- **Probes (`P0`).** A `P0` item is an early experiment that settles an unknown before the real
  work starts. Probe output lives in `docs/plans/probe-results/`.
- **Version string.** `version.txt` holds `MAJOR.MILESTONE.SUB`, with a `-release` suffix when a
  milestone is released. CMake reads it and embeds it in the binary.
- **Git.** The default branch is `master`. Milestone branches (`0.3`, `0.4`, `0.5`) were used while
  each milestone was open. The `0.5` branch is now an ancestor of `master`, and the 0.6 work is
  committed on `master`. Check `git branch` and `git log` for the current state.
- **Layer rule.** Code is split into layers with a strict include direction. Read
  [architecture.md](architecture.md) before adding an `#include`.

## Known documentation drift

These are places where the written status is behind the code. When in doubt, trust the code, the
`version.txt` file and the roadmap, in that order.

- `version.txt` says `0.5.8`, but `master` has the 0.6.1 to 0.6.5 commits. Under the versioning
  rule in [../roadmap.md](../roadmap.md), the version should name the last completed sub-milestone.
  It is not bumped here because a version bump is the owner's call.
- The 0.5 release has no tag. Its verification is recorded in
  [../plans/0.5.md](../plans/0.5.md) and [../plans/probe-results/compile_0_5_release.md](../plans/probe-results/compile_0_5_release.md).
