# Addons P2: where Strata mounts addons (2026-10-24)

Settles [addons.md](../addons.md) P2 for 0.3.7 (0.3 D19). P2CE 0.0.8.2 (steam.inf `PatchVersion`), on Linux.
All test content was Apex-authored: one-line `UnlitGeneric` VMTs and 16×16 single-colour VTFs.

## Game (native Linux build)

**Method.** I ran a private game directory outside the install, so the real addon lists stayed untouched.
It held a copy of `gameinfo.txt` with absolute paths, the P2CE `steam.inf`, symlinks to the rest of `p2ce/`, and
`addons/` with three test addons and a hand-written `addonlist.kv3`. The game ran without Steam inside a
headless compositor:

```
kwin_wayland --virtual --xwayland --exit-with-session "<script running p2ce -game <dir> -condebug -novid -windowed -nosound +path +quit>"
```

Lavapipe was used (`VK_ICD_FILENAMES=…/lvp_icd.json`). Plain Xvfb doesn't work, because DXVK divides by its 0 Hz
refresh rate in `DxgiOutput::CacheMonitorData` (SIGFPE). The `path` output in `console.log` gives the order.

**Result.** List `a (loose), b (loose), c (vpks = [pak01_dir.vpk])`, all mounted:

```
addon  …/addons/addon_a/          ← highest
GAME   …/addons/addon_a/
addon  …/addons/addon_b/
GAME   …/addons/addon_b/
addon  "addon" (VPK) …/addons/addon_c/pak01.vpk
GAME   "GAME" (VPK) …/addons/addon_c/pak01.vpk
addon  …/addons/addon_c/
GAME   …/addons/addon_c/
…      SHADERS, then custom/*, the mod, p2ce, hammer, portal2 …
```

- Addons sit above every gameinfo path, `custom/*` included. They're added at runtime with
  `AddSearchPath(…, PATH_ADD_TO_HEAD)` on the `GAME` and `addon` path IDs (`workshop.so`), after the file
  system has loaded gameinfo and `cfg/mounts.kv`.
- **The first mounted entry in the list wins.** With the list rewritten to `c, b (mounted = false), a`, the order
  became c's VPK, c's folder, then a. b was skipped.
- Within one addon, the listed VPKs sit **above** the addon's folder. The folder is mounted loose even when
  `vpks` isn't empty.
- On start, `workshop.so` rewrote the list: it kept the existing entries and their order, and appended every
  Workshop item in the Steam library (found even with Steam not running) as `mounted = false`.

## Strata Hammer (Windows build under Wine)

**Method.** I backed up `hammer/addons/addonlist.kv3` and pointed it at the test addons with `z:\…` lower-cased
paths. Hammer opened a room map whose faces all use `apexprobe/probe` (`tools/hammer_wine_check.py`'s setup:
Xvfb, lavapipe). I sampled the camera view's colour, then restored the list (SHA-256 checked).

| List (first = top) | Colour | Meaning |
|---|---|---|
| a (red), b (green), c (VPK blue, folder yellow), tex | red | first entry wins |
| c, b, a, tex | **yellow** | c's folder beats c's VPK |
| c (unmounted), b, a, tex | green | `mounted = false` skipped |
| d (VPK only, magenta), tex | magenta | Hammer does mount addon VPKs |
| e (overrides `tools/toolsnodraw`), tex | red | addons beat `p2ce`/`hammer` base content |

- Between addons, Hammer matches the game: the first mounted entry wins.
- **Within an addon, Hammer differs from the game: the loose folder beats the addon's own VPKs.**
- Hammer also rewrote its list on start, appending the local `p2ce/addons/*` folders as `mounted = false`.
  It never lists Steam Workshop items (the owner reports that Wine Hammer can't reach Steam). On this machine its
  list therefore holds only `local` entries.

## Consequences for Apex (0.3 D19, addons D3)

- The addon layer goes above every other mount, in list order, with the first entry highest.
- The order within an addon follows the list being used: Hammer's list puts the folder first; the game's list
  (and Apex's own list) puts the VPKs first.
- Multiple VPKs in one addon weren't measured. Apex keeps them in listed order.
