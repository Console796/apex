# apex.ui/1: panels, sections and dialogs

A plugin describes a form; the host draws it with native widgets. The plugin never sees a widget, and all
callbacks run on the UI thread.

<!-- snippet: include/apex/apex_plugin.h -->
```c
typedef struct apex_ui_v1 {
    uint32_t struct_size;
    uint32_t reserved;
    apex_status (*register_panel)(void* host, apex_str id, apex_str title, void* user, const apex_ui_callbacks*);
    apex_status (*refresh)(void* host, apex_str id);
    /* target: "materials", "inspector" or "face_properties". */
    apex_status (*register_section)(void* host, apex_str id, apex_str title, apex_str target, void* user, const apex_ui_callbacks*);
    /* Modal: returns once the user closes it; *accepted is 1 for OK. Callbacks run while it is open. */
    apex_status (*show_dialog)(void* host, apex_str title, void* user, const apex_ui_callbacks*, uint32_t* accepted);
} apex_ui_v1;
```

**Callbacks.** One `apex_ui_callbacks` table serves all three uses. `describe(user, context, &form)` returns the
current `apex_ui_form` (an array of `apex_ui_control`). `on_change(user, key, value)` reports an edited
control and `on_click(user, key)` a pressed button. The form, its strings and the value are borrowed: the
host copies what it needs before `describe` returns, and the plugin must keep the description alive only
until the next call to `describe`. `context` is `NULL` for panels and dialogs and set for sections.

**Controls.** Every control has a stable `key` (unique within its form; it identifies the control across
re-describes), a `label` and a `value` (`text`, `number` or `checked`, depending on the kind).

| Kind | Widget | Value | Notes |
|------|--------|-------|-------|
| `APEX_UI_LABEL` | read-only text | `text` | |
| `APEX_UI_TEXT` | line edit | `text` | `on_change` fires as the user types |
| `APEX_UI_NUMBER` | spin box | `number` | `minimum` / `maximum` bound it |
| `APEX_UI_CHECKBOX` | check box | `checked` | `label` is the box text |
| `APEX_UI_CHOICE` | drop-down | `text` | `options` lists the entries; `text` is the selected one |
| `APEX_UI_COLOUR` | colour button | `text`, `#aarrggbb` | opens the colour dialog; the button shows the value |
| `APEX_UI_BUTTON` | push button | none | reports through `on_click` |
| `APEX_UI_TABLE` | list of rows | `text` | `options` are the rows; `text` the selected row |
| `APEX_UI_MATERIAL` | editable picker | `text` | material name; `options` are suggestions |
| `APEX_UI_MODEL` | editable picker | `text` | model path |
| `APEX_UI_SOUND` | editable picker | `text` | sound path |
| `APEX_UI_SECTION` | collapsible group | none | `children` / `child_count` hold nested controls |

Only `APEX_UI_SECTION` may have children. Set `struct_size` on every control and on every value, and always
fill `kind`; a description the host cannot read is rejected as a whole and the previous widgets stay.

**Re-describing.** Call `refresh(host, id)` whenever your state changes (also from inside `on_change` or
`on_click`). The host queues it to the event loop and coalesces repeated requests, then calls `describe`
again. The host matches controls by `key` and updates them in place, so widgets, the cursor and keyboard
focus survive a re-describe. A control whose `kind` changes is rebuilt, and one whose key disappears is
removed. Describing a value the user has just typed back unchanged is a no-op; describing a different one
replaces what the user typed, so keep the state your form reports.

**Panels.** `register_panel(host, id, title, user, callbacks)` is available only during `activate`. `id` must be
`plugin.<plugin id>.<name>`. The panel appears in every area's role menu under `title`, lives in the same
dockable area system as the built-in panels and is saved in the layout by `id`. If a saved layout names a
panel whose plugin is missing, disabled or failed to load, the area shows a placeholder naming the panel
and the layout keeps the entry; when the plugin is loaded again the placeholder becomes the real panel.
Plugin panels dock in the main window; they cannot yet be popped out as separate floating windows.

**Sections.** `register_section(host, id, title, target, user, callbacks)` (also `activate` only) adds a titled
group to a built-in panel. `target` is `"materials"`, `"inspector"` or `"face_properties"`; sections are not
area roles and are not saved in layouts. The host calls `describe` with an `apex_ui_context` that names the
panel's current selection and again whenever it changes:

| Target | `context->kind` | Selection |
|--------|-----------------|-----------|
| `materials` | `"materials"` | `names`: the selected material names |
| `inspector` | `"entities"` | `ids`: the selected entity node IDs |
| `face_properties` | `"sides"` | `ids`: the selected side IDs |

Context arrays are borrowed until `describe` returns. An empty selection gives zero counts; the section stays
visible, so describe what it should show then.

**Dialogs.** `show_dialog(host, title, user, callbacks, &accepted)` opens a modal OK / Cancel dialog built from
`describe`, valid only from inside one of your callbacks or `activate`. `on_change` and `on_click` run while it
is open; the call returns when the user closes it and `*accepted` is 1 for OK. Read your own state to find out
what to apply, since the host only reports the decision.

**Errors and failure.** Calls return `APEX_ERR_INVALID` for a bad id, target or table, `APEX_ERR_STATE` outside the
allowed window, `APEX_ERR_NOT_FOUND` for an unknown id and `APEX_ERR_UNSUPPORTED` when the host has no UI
(for example a headless run). An exception, or a non-OK status from `describe`, leaves the existing widgets
as they were; `last_error` holds the detail.

The face-stats example registers a section and describes it:

<!-- snippet: examples/face-stats/face_stats.cpp -->
```cpp
class Stats final : public apex::plugin::Form {
public:
    void describe(const apex_ui_context* context, std::vector<apex_ui_control>& controls) override {
        const bool sides = context && apex::plugin::view(context->kind) == "sides";
        const size_t faces = sides ? context->id_count : 0;
        rows_[0] = faces ? "Faces selected: " + std::to_string(faces) : "Select faces to see their stats";
        rows_[1].clear(); rows_[2].clear();
        if (faces) {
            const auto [low, high] = std::minmax_element(context->ids, context->ids + faces);
            rows_[1] = "Side IDs: " + std::to_string(*low) + (*low == *high ? "" : " to " + std::to_string(*high));
            size_t solids = 0;
            rows_[2] = countNodes(*host, "solid", solids) ? "Solids in the map: " + std::to_string(solids) : "No map is open";
        }
        for (size_t i = 0; i < rows_.size(); ++i) {
            if (rows_[i].empty()) continue;
            auto label = Form::control(APEX_UI_LABEL, keys[i], "");
            label.value.text = apex::plugin::string(rows_[i]);
            controls.push_back(label);
        }
    }
private:
    // The host borrows the text until the next describe(), so it lives here.
    std::array<std::string, 3> rows_;
    static constexpr std::array<const char*, 3> keys{"row0", "row1", "row2"};
};
```

<!-- snippet: examples/face-stats/face_stats.cpp -->
```cpp
apex_status activate(void*) {
    return apex::plugin::registerSection(*host, sectionId, "Face stats", "face_properties", stats);
}
```

## Themes

A plugin ships themes as `themes/*.json` in its folder, in the same format as the user's custom
  themes (`name`, `base`, `colors`). Enabled, loaded plugins' themes are registered as `plugin:<plugin id>:<file
  stem>` and listed in the theme choice as "<name> (plugin <plugin name>)". They are read-only (Edit makes a
  copy as a custom theme). A file that does not parse is logged and skipped. If the plugin is absent or
  disabled its themes are not registered and a saved choice falls back to the default theme.
