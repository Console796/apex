# Source game support

This page says which Source-engine games Apex has been checked against. The answer is short:
**only Portal 2: Community Edition (P2CE) is tested.** Every other game in the table below is
untested. Nothing here means a game is unsupported, and nothing means it works. Untested games may
open, edit or compile correctly, but no one has checked that they do.

## Status terms

| Status | Meaning |
|---|---|
| **Tested** | Apex has been run against the game's own content. The corpus tests, the compile gate and the owner's playtests cover it. Only P2CE is in this state. |
| **Untested** | Apex has not been checked against the game. Its maps may or may not load, and its assets may or may not resolve. |
| **Not planned** | The game's engine or map format is outside the roadmap. |

## Why only P2CE

The roadmap ([roadmap.md](roadmap.md)) and the design rules in the [README](../README.md#design-rules)
set one game profile through 0.10: Strata Source, as P2CE ships it. Game specifics such as the BSP
version, `point_data`, the `light_rt` modes and the mount system live in that profile. Other
games need their own profile, and Apex does not have one yet. The research notes
([research.md](research.md)) name the next targets: other Strata games, then older Source branches
through the same profile layer.

Source 2 is not on the roadmap. Its maps use the `.vmap` format, not the `.vmf` format that
Apex reads and writes.

## Games

The table lists each game that Apex might be asked about. Engine labels follow the family a game
belongs to, not a specific engine build.

| Game | Engine | Apex status | Notes |
|---|---|---|---|
| Portal 2: Community Edition | Strata Source | **Tested** | The only game profile and the test corpus. See [development.md](development.md#4-environment-for-the-full-gate). |
| Portal 2 (Steam) | Source | Untested | The base game that P2CE forks. P2CE's tests say nothing about its content or its tools. |
| Portal: Revolution | Strata Source | Untested | Named as a secondary Strata target in [research.md](research.md). |
| Momentum Mod | Strata Source | Untested | Named as a secondary Strata target in [research.md](research.md). |
| Portal | Source | Untested | |
| Half-Life 2 (with Episodes One and Two) | Source | Untested | |
| Half-Life 2: Deathmatch | Source | Untested | Ships with Half-Life 2. |
| Team Fortress 2 | Source | Untested | |
| Counter-Strike: Source | Source | Untested | |
| Day of Defeat: Source | Source | Untested | |
| Left 4 Dead | Source | Untested | |
| Left 4 Dead 2 | Source | Untested | |
| Alien Swarm | Source | Untested | |
| Alien Swarm: Reactive Drop | Source | Untested | Community project built on Alien Swarm. |
| Garry's Mod | Source | Untested | |
| Black Mesa | Source | Untested | Community remake of Half-Life. |
| Insurgency | Source | Untested | |
| Counter-Strike 2 | Source 2 | Not planned | Source 2 maps are `.vmap`. |
| Dota 2 | Source 2 | Not planned | Source 2 maps are `.vmap`. |
| Half-Life: Alyx | Source 2 | Not planned | Source 2 maps are `.vmap`. |

This list covers the games the project has been asked about or that share the Source map format.
It is not a full catalogue of every game built on Source.

## What moving a game to Tested involves

A game can move from Untested to Tested only when all of the following are true:

1. A game profile exists for it: its `gameinfo`, its mounts and its search paths.
2. Its maps open, round-trip byte-identically when unmodified, and render.
3. Its compile tools have been run on a test map, with the diagnostics and the leak overlay checked
   against real logs.
4. The owner has run the result in the game, because a test does not show that a map plays correctly.

Content from any game is never committed to this repository. Test maps and fixtures come from the
game's install, read through an environment variable, as described in [development.md](development.md).
