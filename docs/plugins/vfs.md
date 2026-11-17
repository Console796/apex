# apex.vfs/1: serve files into the game's file system

`register_provider(host, id, name, provider, position)` is available only during `activate`. `id` is
`plugin.<plugin>.<name>`; at most 16 providers exist in a session; a duplicate id returns `APEX_ERR_INVALID`; a
call after `activate` returns `APEX_ERR_STATE`. The provider becomes a mount of the editor's virtual file system
(Vfs), next to the game, its addons and its archives, so what it serves reaches the material browser, thumbnails,
model and texture loading and the 3D view like any other file.

- **Order.** Earlier mounts win. `position` is `first`, `last` (the default), `before` or `after` an anchor, a
  case-insensitive substring of another mount's folder or archive path. An anchor that matches nothing places the
  provider last and is logged. Providers at the same position keep registration order.
- **Calls.** `list(user, file_sink)` adds every file the provider has (relative paths, `/` separators, any case; at
  most 200,000 paths of 1,024 bytes each). `refresh(user)` is optional and runs before each listing. `open(user,
  path, data_sink)` writes the file's bytes with `data_sink->write`; stop at `data_sink->max_bytes`. Apex lists
  once per load or refresh and then answers `exists`, `list` and directory walks from that index, so only `open`
  reaches your code afterwards.
- **Threads.** The host calls providers from worker threads, concurrently (`open` may run on several threads at
  once; `list` and `refresh` never overlap an `open` of the same provider). Make `open` thread-safe, do not call
  host interfaces from a provider call, and do not rely on the crash breadcrumb: it is not set for provider calls,
  so a plugin that crashes inside one is not named at the next start.
- **Failures.** Return a non-`APEX_OK` status or throw to say "no such file" or "no files"; neither crashes or
  hangs Apex. Each call is bounded (5 s by default). A call that exceeds it fails, and the provider is taken out of
  service until it has returned and the content is next reloaded. The stuck call keeps running on its own thread;
  Apex does not kill it. If it is still running at shutdown, the plugin library is left loaded instead of unloaded.
- **Changes.** `request_refresh(host, id)` (editor thread, your own provider only) reloads the game content so the
  provider is listed again. It is `APEX_ERR_STATE` when the host has no content to reload.
- **Compiling.** The compile tools (vbsp, vvis, vrad) read the real file system and cannot see a provider. Provider
  mounts are left out of the compile gameinfo; the compile panel and the log name each omitted provider.

<!-- snippet: include/apex/apex_plugin.h -->
```c
typedef struct apex_vfs_file_sink {
    uint32_t struct_size;
    uint32_t reserved;
    void* sink;
    apex_status (*add)(void* sink, apex_str path);
} apex_vfs_file_sink;
typedef struct apex_vfs_data_sink {
    uint32_t struct_size;
    uint32_t reserved;
    void* sink;
    size_t max_bytes;
    apex_status (*write)(void* sink, const void* data, size_t size);
} apex_vfs_data_sink;
typedef struct apex_vfs_provider {
    uint32_t struct_size;
    uint32_t reserved;
    void* user;
    apex_status (*refresh)(void* user); /* optional: prepare for a new listing */
    apex_status (*list)(void* user, const apex_vfs_file_sink* sink);
    apex_status (*open)(void* user, apex_str path, const apex_vfs_data_sink* sink);
} apex_vfs_provider;
typedef struct apex_vfs_position {
    uint32_t struct_size;
    uint32_t kind; /* 0 first, 1 last, 2 before anchor, 3 after anchor */
    apex_str anchor;
} apex_vfs_position;
typedef struct apex_vfs_v1 {
    uint32_t struct_size;
    uint32_t reserved;
    apex_status (*register_provider)(void* host, apex_str id, apex_str name, const apex_vfs_provider* provider,
                                     const apex_vfs_position* position);
    apex_status (*request_refresh)(void* host, apex_str id);
} apex_vfs_v1;
```

<!-- snippet: include/apex/apex_plugin.h -->
```c
enum apex_vfs_position_kind { APEX_VFS_FIRST = 0, APEX_VFS_LAST = 1, APEX_VFS_BEFORE = 2, APEX_VFS_AFTER = 3 };
```

No example plugin serves files; the C++ `FileProvider` base class wraps the table.
