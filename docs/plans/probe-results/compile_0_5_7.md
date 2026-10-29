# 0.5.7: addon targets and authoring — 2026-10-28

## Observed inputs and boundaries

The installed `p2ce/addons/*/addon.kv3` manifests use the text/generic KV3 header, `mod`,
`description`, `type`, numeric `id`, `thumbnail`, `authors`, `dependencies`, `tags`, `ignore`,
`metadata` and `assets`. The scaffold follows that shape, with local ID zero, empty dependency
entries and an optional empty campaign. Campaign scripts in the installed local and Workshop
addons are headerless KV3 and contain a `campaigns` object keyed by campaign ID; this form is
accepted alongside header-bearing KV3. No shipped content is copied into this repository.

The native uploader's help was queried with a temporary gameinfo and steam.inf, outside the
install. It advertises DIRECTORY, interactive mode and update-message options, with no offline
validation subcommand. The directory action was **not** invoked: it could upload. Uploader
acceptance of a new scaffold therefore remains a manual check; complete its thumbnail and
campaign chapters in Strata's tools before publishing. No dependency syntax is inferred (P4).
The GUI hand-off uses no invented addon-selection switch.

## Compile parity

P1's Linux implementation evidence is in `lighting_compilation.md`: native VBSP rejects
`-mountmod`, while temporary gameinfo paths resolve the mounted PBR material and all three
native tools complete. The addon target is prepended to this compile configuration. The
installed gameinfo and addon lists are not edited. A separate game-list comparison reports
extra/missing mounts and ordering differences, plus an unmounted target. The summary identifies
mounted addon provenance and unmounted providers of missing assets.

The real `compile_chamber` gate passed the baseline, cut cordon and created-instance/addon-target
cases: 533 assertions. All compiler outputs stayed in temporary directories. BSP copies reached
the temporary addon's `maps`; no case produced a leak file. Campaign arguments are covered by a
scripted detached runner; the gate never launches the game.

## Environment

Native tools inside the restricted filesystem sandbox waited on a crash-upload consent dialog.
Those owned test processes were stopped without accepting uploads. The host's saved upload
consent was read as revoked (`0`); native validation runs on the host, with `SENTRY_DSN` empty.
Setting that environment variable alone did not resolve the restricted-sandbox startup.
