# apex.actions/1: commands, suggested keys and pies

`register_action(id, name, description, user, invoke, suggestions, n)` adds a command. It appears in the
command palette and the keybinding page. Actions are *optional* in presets: Apex never binds a plugin
action on its own. `apex_action_suggestion{preset, chord, tool_id}` proposes a default binding (empty
`tool_id` = global context). Suggestions apply only when they do not conflict; a user binding always wins.

<!-- snippet: include/apex/apex_plugin.h -->
```c
typedef struct apex_action_suggestion { uint32_t struct_size, reserved; apex_str preset, chord, tool_id; } apex_action_suggestion;
typedef struct apex_actions_v1 {
    uint32_t struct_size;
    uint32_t reserved;
    apex_status (*register_action)(void*, apex_str id, apex_str name, apex_str description,
                                   void* user, apex_callback invoke,
                                   const apex_action_suggestion* suggestions, size_t suggestion_count);
} apex_actions_v1;
```

The hello-tool example registers an action with a suggested Hammer binding on F24:

<!-- snippet: examples/hello-tool/hello.c -->
```c
    suggestion = (apex_action_suggestion){sizeof(apex_action_suggestion), 0, str("Hammer"), str("F24"), str("plugin.hello.tool")};
    return actions->register_action(host->context, str("plugin.hello.read_document"), str("Read document"),
                                    str("Log the root class name"), 0, read_document, &suggestion, 1);
```

Pies, suggested keys and themes use data a plugin already ships; there is no extra C interface.

- **Pies.** A pie layout lives in the user's preset (`pies` in the preset file), keyed by pie id (`pie.tools`,
  `pie.objects`), and is edited on the Pies page of Settings. A preset with no entry for a pie uses the built-in
  layout. An item may name any action id, including `plugin.<id>.<name>`. While the plugin is not loaded that
  item is hidden when the pie opens and shown greyed on the Pies page, but it stays in the preset and reappears
  when the plugin is back. Constraints: slot 0-7 (N, NE, E, SE, S, SW, W, NW), no two items in one slot, at most
  8 items. The preset store refuses to save a layout that breaks them. Plugins cannot add pie items themselves.
- **Suggested keys.** `suggestedBindings` and `suggestedContexts` on an action are keyed by preset name. The
  Keybindings page lists the action under "Plugin: <name>" with the key in italics as "<key> (suggested)".
  A suggestion is not written into the preset; a binding in the preset, even "none", replaces it. If the key
  is already used in the same context the action shows no key and the tooltip names the owner. Suggestions
  exist only for the presets they name, so a user preset copied from one gets none.
