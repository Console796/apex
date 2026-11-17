# Threading and memory rules

These rules apply to every interface. The page for an interface gives the details.

## Threads

| Rule | Detail |
|---|---|
| Plugin callbacks run on the editor (main) thread | `activate`, `deactivate`, `on_document_opened`, `on_document_closing`, `on_changed`, tool, action, UI, helper, problem and importer callbacks, job `done`, watch `on_changed` |
| Exceptions: workers | Job `work` runs on a worker thread. VFS `refresh`, `list` and `open` run on host worker threads, `open` several at once |
| Host calls | Host interface calls are editor-thread only unless the interface page says otherwise. `jobs.submit` and `jobs.cancel` are thread-safe. From a worker callback (`work`, a provider call) the only host calls allowed are the ones named on the page: `cancelled`, `jobs.submit` / `jobs.cancel` (jobs) and `log` |
| Registration | Only inside `activate` (the interface pages say which calls). Later registration returns `APEX_ERR_STATE` |
| Exceptions | Never let a C++ exception cross the boundary. `plugin.hpp` wrappers catch; raw code must too. The host also catches around most callbacks, but treat that as a safety net |
| Blocking | A provider call has a 5 s budget; a script call 250 ms. Nothing else is bounded: do not block the editor thread |

## Ownership and lifetime

- Every `apex_str` or pointer the host hands you is **borrowed**: valid until the callback returns, or, for strings
  returned by `apex.document` and `apex.assets`, until the next call into the same interface or any document
  mutation. Copy what you keep.
- Everything you hand the host is **copied** before the call returns (overlay lists, UI forms, edit arguments), except what the interface page says must outlive it: a `user` pointer
  stays yours and must stay valid until the interface says the host is done with it (a registration's `user` until
  unload, a job's `user` until `done` runs or `cancel` succeeds).
- The `apex_host*` and the interface tables from `get_interface` stay valid until the library is unloaded; `host->context`
  is valid until `deactivate` returns.
- The `apex_plugin` table you return from `apex_plugin_entry` is yours and must stay valid until the host unloads
  the library, after `deactivate`.
- Strings in `apex_str` are UTF-8, not NUL terminated; `size` is the length.
- `host->last_error(context)` returns a human-readable reason for the most recent failure; copy it before the next call.

## Memory

Allocate and free on the same side of the boundary: never free a host pointer, never hand the host a pointer it
should free. Struct arrays you provide are read with a stride of their elements' `struct_size`; arrays the host
provides are read with `apex_array_element(array, i)` ([versioning.md](versioning.md)). Keep per-session tool state
in `create_instance` objects, not globals ([tools.md](tools.md)).
