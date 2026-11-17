# Registration and ids

Everything you register (tool, action, panel, section, overlay, helper, check, importer, provider) has an id of the
form `plugin.<your id>.<name>`. Registration is possible only inside `activate`.

Everything you register is `plugin.<yourid>.<name>`. Your id is the manifest's `id`: dot-separated
parts of letters, digits, `_` and `-`. `<name>` is a single such part with no dot. Matching is on the
`plugin.<yourid>.` boundary, so a plugin `com.example` cannot register `plugin.com.example.paint.brush` (that is
`com.example.paint`'s `brush`). An empty name, a trailing dot, a second dot or any other character fails with
`APEX_ERR_INVALID` at the registration call.

## apex.registration/1

`apex.registration/1` is the first skeleton of the ABI (0.9.1): `action`, `tool` and `panel` take bare callbacks. It is
kept so a plugin written against it still loads. New plugins use `apex.actions/1`, `apex.tools/1` and `apex.ui/1`, which
carry everything a registration needs (suggested keys, per-session tool state, parameters, forms).

<!-- snippet: include/apex/apex_plugin.h -->
```c
typedef struct apex_registration_v1 {
    uint32_t struct_size;
    uint32_t reserved;
    apex_status (*action)(void* host, apex_str id, apex_str name, apex_str description,
                          void* user, apex_callback invoke);
    apex_status (*tool)(void* host, apex_str id, apex_str name, apex_str description,
                        void* user, apex_callback activate, apex_callback deactivate);
    apex_status (*panel)(void* host, apex_str id, apex_str title);
} apex_registration_v1;
```
