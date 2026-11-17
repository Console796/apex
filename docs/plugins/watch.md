# apex.watch/1: file watching

`watch(host, path, user, on_changed, &watch_id)` watches an absolute path to a file or directory (a directory is
watched recursively, up to 10,000 entries); `unwatch(host, watch_id)` stops it. Allowed during `activate` or any
callback into the plugin. The path must lie inside the plugin's own folder or a root the host was given (the game
directory); otherwise the call returns `APEX_ERR_INVALID`.

`on_changed(user, path)` is delivered on the editor thread **once per save**: the host polls size, modification
time and, for directories, the entry set, and delivers only after the path has been quiet for the debounce
period (200 ms by default). A program that writes a file in several pieces, or a burst of saves inside the window,
is one delivery. A separate save afterwards is another. Creating or deleting the watched path counts as a change.
Callbacks may call `unwatch`. Polling means a very coarse filesystem mtime (1 to 2 s on some filesystems) can merge
two saves made within that resolution into one delivery. `Host::pollWatches()` must be called regularly (the Qt
shell uses the same `PluginPump` timer).

<!-- snippet: include/apex/apex_plugin.h -->
```c
typedef void (*apex_watch_callback)(void* user, apex_str path);
typedef struct apex_watch_v1 {
    uint32_t struct_size;
    uint32_t reserved;
    apex_status (*watch)(void* host, apex_str path, void* user, apex_watch_callback on_changed, uint64_t* watch_id);
    apex_status (*unwatch)(void* host, uint64_t watch_id);
} apex_watch_v1;
```

No example plugin watches a file by itself; the script host uses this mechanism to reload scripts.
