# Apex: Strata addon interop: research and cross-milestone plan

**Status: draft, 2026-10-22. The owner accepted the timeline in §4, and its amendments are applied to
[roadmap.md](../roadmap.md), [research.md](../research.md) and [0.3.md](0.3.md) (D19, sub-milestone
0.3.7). Decisions D1 to D10 haven't been reviewed one by one: each milestone's plan confirms the ones
it implements.** This is a cross-milestone design, not a milestone plan.

"Addon" in this document means a **Strata content addon**: a folder of game content (VPKs, loose
files, a campaign script) that the engine and Strata Hammer mount on top of the game. That's a
different thing from the roadmap's 0.9 "addon manager", which manages Apex's own plugins (D10
renames it).

## 1. What Strata does (research, 2026-10-22)

Sources are the Strata wiki (`modding/workshop/*`, `modding/p2ce-campaigns/*`,
`modding/overview/mounts.md`); the P2CE install on the dev machine (35 addon folders, 26
subscribed Workshop items, both `addonlist.kv3` files); and strings in the shipped binaries
(`workshop.dll`/`.so`, `hammer_dll.dll`, `engine.dll`, `launcher.dll`, the compile tools,
`workshopcli`, `workshopgui`). Anything taken from strings and not observed in use is marked
*(strings)* and is unverified.

### 1.1 Addon on disk

```
p2ce/addons/<folder>/            local addon (or <steam library>/steamapps/workshop/content/440000/<id>/)
    addon.kv3                    manifest
    .assets/                     thumbnail and UI art; never mounted as game content
    pak01_dir.vpk, pak01_000.vpk ...   packed content (chunked VPK)
    maps/ materials/ scripts/ ...      or loose content, laid out like a mod folder
    scripts/campaigns.kv3        optional: makes it a campaign
```

`addon.kv3` is **text KV3** (`<!-- kv3 encoding:text:version{e21c7f3c-…} format:generic:version{7412167c-…} -->`).
Keys seen across all 35 manifests:

| Key | Seen | Meaning |
|---|---|---|
| `mod` | 35 | Display name. |
| `description` | 35 | Free text, often Steam BBCode. |
| `type` | 35 | `Asset Pack`, `Map`, `Campaign`/`campaign`, `Other`. Case is inconsistent, so compare case-insensitively. |
| `id` | 34 | Workshop published file ID; `0` for local-only. |
| `thumbnail` | 35 | Path relative to the addon, normally under `.assets/`. |
| `authors`, `tags` | 35 | String arrays. Tags come from a fixed set (`Materials/Textures`, `Models`, `Scripts`, `Game Port`, `Singleplayer`, `Short (1-3 maps)`…). |
| `dependencies` | 35 | Array; empty in every sample. The tools reject an "unexpected dependency type specifier" *(strings)*, so entries carry a type prefix whose syntax isn't known yet. |
| `ignore` | 35 | Array; empty in every sample. The uploader uses it to exclude files from packing *(strings: "ignore block is not an array")*. |
| `metadata`, `assets` | 35 | Empty blocks in every sample. |
| `mounted_by_default` | 30 | Missing means `true`. 26 of the 30 that have it say `true`. |

The wiki tells users not to edit the file by hand, because the uploader is strict about its format.

### 1.2 Mount state: `addonlist.kv3`, one per program family

Mount state doesn't live in the manifest. It lives in an `addons/addonlist.kv3` next to each
program's game directory:

```
{ addons = [ { dir = "<abs path>"  mounted = true|false  type = "local"|"steam"  vpks = [ "pak01_dir.vpk" ] } … ] }
```

On this machine **the game and Hammer keep separate lists, and the two have drifted apart**:

| | `p2ce/addons/addonlist.kv3` (game) | `hammer/addons/addonlist.kv3` (Hammer) |
|---|---|---|
| Entries | 41: 15 `local`, 26 `steam` | 35, all `local` (Hammer runs under Wine without Steam UGC) |
| Path style | Native: `/home/…/workshop/content/440000/3594428478` | Wine and **lower-cased**: `z:\home\taylorg\snap\steam\…\p2ce\addons\ws_3594428478` |
| Mounted | 6, including Workshop item 3506356222 (ApertureVR PBR) | 6, including `p2ce_portal2`; 3506356222 is **not** mounted |

So Hammer under Wine sees only local folders. Here the user worked around that by keeping `ws_<id>`
copies of Workshop items in `p2ce/addons`, which shows up as duplicate IDs (a Steam entry and a
local entry for the same item). `vpks` lists the archives to mount. When it's empty (as for
`p2ce_portal2`, which is loose `scripts/` only), the folder itself is mounted loose.

`workshop.dll` (`IWorkshop001`, the same library for game and Hammer) owns all of this:

- it scans the local `addons/` folder and Steam subscriptions ("Found local addon", "Found Steam
  addon with ID");
- it rebuilds the list ("Could not load addonlist.kv3, creating a new one");
- it uses `mounted_by_default` for addons it hasn't seen before;
- it provides `workshop_mount`, `workshop_unmount`, `workshop_list`, `workshop_flush` and
  `workshop_missing_addons`;
- it refuses to unmount while a map is running in game.

`-noworkshop` turns the system off.

### 1.3 Strata Hammer's Addon Manager

The Addon Manager is under Window → Addon Manager (`ui::CAddonManager`). It's a modal Qt table with
a checkbox and the columns Name, Author, Type (`Game Steam`, `Game Local`) and Description, plus a
Reload button. Ticking a row mounts that addon into Hammer's filesystem, which makes its materials
and models appear in the browsers. That's how the PBR packs' descriptions tell mappers to use them
("filter for 'pbr' in Hammer"). The wiki's campaign workflow builds on this:

1. Create an addon in the SDK launcher.
2. Point a copied game configuration's VMF directory at `<addon>/mapsrc` and its BSP directory at
   `<addon>/maps`.
3. Tick the addon in the Addon Manager.
4. Add `-campaign "<id>"` to the run command.

Campaign IDs are scoped by bucket: `base/x`, `addon:<folder>/x`, `workshop:<id>/x`.

### 1.4 Compile tools

`vbsp`, `vvis`, `vrad`, `vtex`, `bspzip` and `height2ssbump` all embed `addons/addonlist.kv3` and
honour `mounted` *(strings)*. Each one resolves it against its `-game` directory, so a compile
sees **the game's** mount set, not necessarily the one Apex showed. Native verification on
2026-10-28 found that VBSP could not see mounted EverythingPBR content and rejected `-mountmod`.
A temporary compile-only gameinfo with explicit VFS search paths resolved the missing material
and removed its erroneous `SURF_NOLIGHT` flag; see `probe-results/lighting_compilation.md`.

### 1.5 What Apex does today

Nothing. `Vfs::load` mounts gameinfo `SearchPaths` (including `custom/*`) and `cfg/mounts.kv`. It
never reads either addon list, and `core` has no KV3 parser. Content that only exists in an addon
is therefore missing in Apex. A map that uses EverythingPBR or ApertureVR materials opens with
error textures, and the material browser (0.3 D14) wouldn't list them. `custom/*` globbing doesn't
cover addons: they're a separate mount layer.

## 2. Open facts to probe (the milestone in brackets settles each one)

| # | Question | How | Settles |
|---|---|---|---|
| P1 (0.5, Linux compile parity verified: D8) | Which list does each compile tool read? Does `-mountmod` add an addon? | Build a map that uses a material found only in an unmounted addon, compile it with the game list toggled each way, and read the result with `vbspinfo` or the vbsp log. | D8 |
| P2 (0.3.7 P0, **done**: D3) | Where do mounted addons sit in the search order relative to `custom/*`, the mod, `hammer`, and `mounts.kv` heads? Do they mount in list order or reverse? | Use two test addons and a `custom/` folder, each overriding the same VMT with a different `$basetexture`. Check what game (`path`, `mat_texture_list`) and Hammer show. | D3 |
| P3 (0.4) | Does Hammer rewrite its list completely or edit it in place? Does it pick up an external edit while running, or only on Reload? | Toggle in Apex while Hammer runs, then Reload. Toggle in Hammer, then diff. | D2 |
| P4 (0.5) | What syntax does a `dependencies` entry use? | `workshopcli` with a dependency set, or the uploader GUI. | D5 |
| P5 (0.3.7, if the VM runs) | Does Hammer on native Windows write `C:\…` with the case preserved? | Windows VM, only when it's next booted for a milestone tag (it isn't worth a dedicated run). | D4 |

## 3. Decisions (drafts for review)

### D1: Addons are a profile-owned mount layer in `Vfs`, not a plugin feature

The P2CE `GameProfile` gains an `addons` section with these fields:

- `list`: which `addonlist.kv3` to follow;
- `roots`: the local addons folder and the Workshop content folder, found through steampp;
- `enabled`: on by default.

`Vfs::load` mounts each mounted addon's `vpks` in listed order, followed by the addon folder loose,
at the position P2 establishes. `.assets/` is never mounted. Every mount records the addon it came
from, so `Vfs` can answer "which addon provides this path". This follows roadmap principle 5:
addons are a Strata quirk, kept behind the profile.

### D2: Apex follows Strata Hammer's list by default and writes back only the `mounted` flag

Apex and Hammer are meant to be interchangeable on the same maps (principle 2), so by default
they should see the same content. Apex reads `hammer/addons/addonlist.kv3` and falls back to the
game's list when Hammer's doesn't exist. When the user toggles an addon in Apex, Apex flips that
entry's `mounted` value **in place, byte for byte**: it changes no other text, rewrites no paths
and adds no entries, the same way it treats unknown VMF content. Entries for addons the list
doesn't know about are left to `workshop.dll` to add. Apex shows them as "not in list" and mounts
them according to `mounted_by_default` until the next time Hammer rewrites the file. Apex watches
the file and reloads when it changes (P3).

The alternatives were Apex's own list in the profile (simpler, but drifts from Hammer exactly as
the game list already has) and following the game's list (it matches what compiles see, but not
what Hammer shows). The profile setting allows `hammer`, `game` or `apex`, so a user can choose.

### D3: Search order matches the engine (measured by P2, 2026-10-24)

*P2 result ([probe-results/addons_p2.md](probe-results/addons_p2.md)):* addons sit above every other
search path, `custom/*` included, and the first mounted entry in the list has the highest priority, in
both the game and Strata Hammer. Within one addon the programs differ: the game puts the listed VPKs
above the addon's loose folder, and Hammer puts the folder above the VPKs. Apex orders an addon's mounts
the way the program whose list it follows does. The draft guess ("just above the mod and below
`custom/*`") was wrong.

### D4: Paths in the list are normalised on read and kept verbatim on write

Apex maps `z:\` to `/` and `<letter>:\` to that drive (on Windows), then resolves the result with
the existing ASCII case-insensitive `expand()`. Wine Hammer lower-cases every path, and Linux is
case-sensitive. Entries that can't be resolved are listed as missing, never deleted.

### D5: A text-only KV3 reader in `core`, with span-preserving edits

The reader parses the header, blocks, arrays, strings (including multi-line `"""`), numbers,
booleans, `null` and comments, and records each value's byte span so D2 can replace one value.
Binary KV3 isn't needed: every file Strata leaves on disk for addons, campaigns and cvar defaults
is text. Unknown manifest keys and dependency syntax are kept, not interpreted, until P4 answers.

### D6: The Addons panel is a panel role that does what Hammer's dialog does, and more

Like the material browser (0.3 D14), the panel is a dockable role, not a modal. It has:

- a mounted checkbox, thumbnail (from `.assets`), name, authors, type, source (Local/Workshop),
  Workshop ID and tags;
- a filter, and Reload.

On top of what Hammer offers, it adds:

1. **Duplicate-ID warnings** for local and Workshop copies of the same item.
2. **Missing-path warnings**.
3. **"Provided by an unmounted addon" hints.** Apex indexes the file lists of unmounted addons
   (VPK directories only, which is cheap). When a face or model in the map refers to an asset that
   only an unmounted addon has, the error-texture tooltip and the entity report name that addon
   and offer to mount it.

Toggling an addon rebuilds the Vfs off the main thread. Once that's done, it invalidates
`MaterialCache` and the material index, as a mount change.

### D7: The map does not record its addons

Neither the VMF nor Hammer records which addons a map needs, and Apex mustn't add anything to the
VMF that Hammer would drop or rewrite (0.1 D19). "Addons this map uses" is computed on demand from
the Vfs provenance (D1). It appears in the entity report (0.4) and the compile summary (0.5).

### D8: Compiles see what the editor sees

When addons are mounted, Apex writes a temporary compile-only gameinfo containing the ordered
VFS mounts and anchored engine metadata. Compile tools use that directory; BSP installation and
game launches keep the original game directory. The installed gameinfo and addon list remain
untouched. Native verification is recorded in `probe-results/lighting_compilation.md`.

### D9: Authoring stops at scaffolding and handoff; uploading stays with Strata's uploader

From Apex, the user can:

- create an addon: write `addon.kv3` in the uploader's format and create `mapsrc/` and `maps/`;
- open one as a compile target, so VMFs save to `mapsrc` and BSPs go to `maps`;
- launch with `-campaign addon:<folder>/<id>`;
- open the addon in `workshopgui`.

Apex doesn't upload, doesn't edit `campaigns.kv3` beyond creating a stub, and doesn't pack VPKs.
Strata's tools already do all three, and the Workshop flow needs Steam credentials Apex shouldn't
handle.

### D10: The roadmap's 0.9 "addon manager" becomes "plugin manager"

This removes the name clash with Strata's content addons, which ship in 0.3 to 0.5 under this
plan.

## 4. Where it fits in the timeline (recommendation)

| Part | Milestone | Why there |
|---|---|---|
| **Read-only addon mounting**: D1, D3, D4 and D5, plus following Hammer's list (D2's read half) and the P2/P3 probes. | **0.3: sub-milestone 0.3.7 (0.3 D19), before the material index (now 0.3.8).** | 0.3 D14 indexes every material across P2CE's mounts. Built without addons, it would index the wrong set, and then need reworking as soon as addons arrive. On this machine the two largest material sources mappers use (EverythingPBR at about 1.9 GB, ApertureVR PBR at 600 MB) are addons. It's a small piece of work: one parser, one extra mount layer, and fixtures. The plan labels for the material index and face editing moved from 0.3.7 and 0.3.8 to 0.3.8 and 0.3.9. Neither had started. |
| **Addons panel**: D6 and the "unmounted addon" hints. D2's write-back moved into 0.3.7 as the Addons menu at the owner's request (2026-10-24). | **0.4**, next to the model, sound and particle browsers. | It's a browser panel of the same kind. The hints need the 0.4 entity report and model loading to pay off. |
| **Compile parity and authoring**: D7's report, D8 and D9, plus the P1 probe. | **0.5**, Compile & play. | That's where compile profiles, `-game` and the run command are built. The addon-as-compile-target workflow from the wiki belongs with them. |
| **Interop regression**: the addon list round trip, and both editors open with the same mounts. | **0.10** interop suite. | Same treatment as the VMF interop tests. |
| **Rename** (D10). | Roadmap edit now. | Avoids confusion in the meantime. |

Spec amendments (applied 2026-10-22):

- roadmap.md: add addon mounting to 0.3, the Addons panel to 0.4, and addon compile and authoring
  to 0.5; rename the item in 0.9.
- research.md: add an "Addons" row to the format table (1.1–1.4 condensed).
- 0.3.md: add the new sub-milestone, and widen D14's index to include addon mounts.


### 0.5.7 as built (2026-10-28)

P1 is answered for this host: native VBSP rejected `-mountmod`; the temporary-gameinfo mount
strategy in D8 is verified in `probe-results/lighting_compilation.md`. Compile targets prepend the
selected addon to that compile configuration. The game still reads its own mount list; the compile
panel names differences and an unmounted target instead of silently rewriting the list.

The profile stores the target and campaign ID. Save As defaults to `mapsrc`, BSP installation
goes to `maps`, and detached launches add `-campaign addon:<folder>/<id>` read from
`scripts/campaigns.kv3` (including the headerless form shipped by P2CE). The scaffold creates an
empty campaign only on request and never writes nonempty dependencies, so P4 remains deferred.
Uploader hand-off starts the native executable without an unverified selection argument.
