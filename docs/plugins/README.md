# Apex plugin SDK

## What a plugin is

A plugin is a native shared library (`.so` on Linux, `.dll` on Windows) loaded in-process, plus a
`plugin.kv3` manifest. It talks to Apex only through a versioned C ABI in
`include/apex/apex_plugin.h`. A header-only C++ wrapper,
`include/apex/plugin.hpp`, adds exception-safe base classes on top.

Plugins run inside the editor process with no sandbox. A crash in a plugin crashes Apex; see
[safe mode](getting-started.md#crashes-and-safe-mode).

The ABI is **major 1, minor 0** and is frozen: see [versioning.md](versioning.md).

## Pages

| Page | What it covers |
|---|---|
| [getting-started.md](getting-started.md) | The SDK, building with `find_package(ApexPlugin)`, the manifest, the entry point and lifecycle, safe mode |
| [cpp-wrapper.md](cpp-wrapper.md) | `plugin.hpp`, the header-only C++ layer |
| [threading-and-memory.md](threading-and-memory.md) | Which thread each callback runs on, who owns which pointer, how long it stays valid |
| [versioning.md](versioning.md) | ABI major and minor, the evolution rules, what is frozen |
| [examples.md](examples.md) | hello-tool, polygon-tool, face-stats, obj-import and the stair script |

Interfaces (each is fetched by name with `host->get_interface`):

| Page | Interface |
|---|---|
| [document.md](document.md) | `apex.document/1`: read the document |
| [registration.md](registration.md) | Ids, namespaces, and the 0.9.1 `apex.registration/1` skeleton |
| [tools.md](tools.md) | `apex.tools/1`: viewport tools |
| [actions.md](actions.md) | `apex.actions/1`: commands, suggested keys and pies |
| [edit.md](edit.md) | `apex.edit/1`: writing to the document, change notices |
| [selection.md](selection.md) | `apex.selection/1` |
| [ui.md](ui.md) | `apex.ui/1`: panels, sections, dialogs; themes |
| [overlay.md](overlay.md) | `apex.overlay/1` |
| [helpers.md](helpers.md) | `apex.helpers/1`: FGD helper drawing |
| [problems.md](problems.md) | `apex.problems/1` |
| [io.md](io.md) | `apex.io/1`: importers |
| [jobs.md](jobs.md) | `apex.jobs/1`: background work |
| [watch.md](watch.md) | `apex.watch/1`: file watching |
| [vfs.md](vfs.md) | `apex.vfs/1`: serve files into the game's file system |
| [assets.md](assets.md) | `apex.assets/1`: materials |
| [script-host.md](script-host.md) | Script plugins (AngelScript) |

Every code block marked `snippet:` in these pages is copied from the file it names; `tools/check_docs.py`
checks that in CI. Design rationale lives in the Apex repository, `docs/plans/plugins.md`.
