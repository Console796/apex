# Getting started

Build a plugin from the SDK alone: no Apex source tree, no Qt. The SDK is the `sdk` component of an Apex
build, shipped as `apex-plugin-sdk-<version>.zip` (one archive for every platform; it is headers and CMake only).

## The SDK

```
apex-plugin-sdk-<version>/
  include/apex/apex_plugin.h, plugin.hpp    the C ABI and the C++ wrapper
  lib/cmake/ApexPlugin/                     find_package(ApexPlugin): apex::plugin_sdk and apex_add_plugin()
  share/apex-sdk/examples/                  the example plugins and the CMake project that builds them
  share/doc/apex-sdk/                       these pages
  LICENSE
```

## Building

`find_package(ApexPlugin)` defines the imported target `apex::plugin_sdk` (the include directory) and the
function `apex_add_plugin`. It builds a module library with no `lib` prefix and hidden symbols except
`apex_plugin_entry`, writes `plugin.kv3` for you, and puts both in the layout below. C++ sources get C++23.

<!-- snippet: examples/CMakeLists.txt -->
```cmake
cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(apex_plugin_examples LANGUAGES C CXX)
  find_package(ApexPlugin REQUIRED)
endif()
```

<!-- snippet: examples/CMakeLists.txt -->
```cmake
  apex_add_plugin(hello_tool ID hello NAME "Hello tool" FOLDER hello SOURCES hello-tool/hello.c)
```

Configure with the SDK on `CMAKE_PREFIX_PATH`, for instance
`cmake -S <sdk>/share/apex-sdk/examples -B build -DCMAKE_PREFIX_PATH=<sdk>`, and build. The result is a plugin
folder (`<build>/plugins/hello/`). Copy it into Apex's `plugins` directory (next to the executable, or the data
directory) and restart.

`apex_add_plugin` arguments: `ID`, `NAME` and `SOURCES` are required; `VERSION` (default `1.0.0`), `FOLDER`
(default the id), `OUTPUT_DIRECTORY` (default `${APEX_PLUGIN_OUTPUT_DIRECTORY}`, else `<build>/plugins`) and
`ABI_MAJOR` / `ABI_MINOR` (default 1 and 0) are optional. `apex_add_script_plugin` takes `SCRIPT` and `RUNTIME`
instead of `SOURCES`. Build with a compiler and standard library compatible with the Apex release you target;
only the C ABI is guaranteed across compilers, so keep C++ types out of the boundary.

## Layout and manifest

Each plugin lives in its own directory under `<data>/plugins/` (or the bundled plugins directory). The layout `apex_add_plugin` produces:

```
plugins/hello/
  plugin.kv3
  bin/linux-x86_64/hello_tool.so
  bin/windows-x86_64/hello_tool.dll   # a folder built on each platform can be merged
```

`plugin.kv3`:

```
<!-- kv3 encoding:text:version{apex} format:generic:version{fixture} -->
{
    id = "hello"
    name = "Hello tool"
    version = "1.0.0"
    apex_abi = { major = 1 minor = 0 }
    entry = {
        "linux-x86_64"   = "bin/linux-x86_64/hello_tool.so"
        "windows-x86_64" = "bin/windows-x86_64/hello_tool.dll"
    }
}
```

| Field | Meaning |
|---|---|
| `id` | Plugin namespace. Everything you register must be prefixed `plugin.<id>.`. |
| `name`, `version` | Shown in the plugin manager. |
| `apex_abi` | The ABI you were built against. Major must equal the host's; minor must be ≤ the host's. |
| `entry` | Library file per platform key. A plugin with no entry for the current platform is skipped. |
| `script`, `runtime` | A script plugin ([script-host.md](script-host.md)) names a script file inside its folder and the runtime that runs it, instead of `entry`. A plugin has `entry` or `script`, never both. |

## Entry point and lifecycle

Export one symbol:

`const apex_plugin* apex_plugin_entry(const apex_host* host)`, declared `APEX_PLUGIN_EXPORT` so it is visible
in the library (see the definition in `hello.c` below).

Apex calls it once. Check `host->abi_major`/`abi_minor`, stash whatever you need from `host`, and return a
pointer to a static `apex_plugin` table. Always set `struct_size = sizeof(...)` on every struct you fill in;
the host uses it to tell older and newer layouts apart.

| Callback | When |
|---|---|
| `activate(user)` | After the handshake, before the main window. **Registration is only valid here.** Return `APEX_OK`. |
| `on_document_opened(user)` / `on_document_closing(user)` | A document became / is about to stop being current. |
| `deactivate(user)` | Shutdown or when the plugin is disabled. |

Rules that apply to every callback:

- Never let a C++ exception cross the boundary. The `plugin.hpp` wrappers catch for you; raw C++ code must too.
- Returned `apex_str` values are borrowed. Copy them if you need them past the next call into the same
  interface or any document mutation.
- Failures return an `apex_status`; `host->last_error(context)` gives a human-readable reason for the
  most recent failure.
- `host->log(context, level, message)` writes to the Apex log, tagged with your plugin id.

The host refuses a plugin whose `apex_plugin.struct_size` does not cover `activate`, whose `abi_major` differs
from the host's, or whose `abi_minor` is above the host's. Return `NULL` from the entry point to refuse to load.

<!-- snippet: examples/hello-tool/hello.c -->
```c
static apex_status activate(void* user) {
    host->log(host->context, 1, str("Hello from hello-tool"));
    const apex_tools_v1* tools;
    const apex_actions_v1* actions;
    apex_action_suggestion suggestion;
    apex_status status;
    read_document(user);
    tools = (const apex_tools_v1*)host->get_interface(host->context, str("apex.tools/1"));
    actions = (const apex_actions_v1*)host->get_interface(host->context, str("apex.actions/1"));
    if (!tools || !actions) return APEX_ERR_UNSUPPORTED;
    status = tools->register_tool(host->context, str("plugin.hello.tool"), str("Hello tool"),
                                  str("Preview a point marker"), 0, &tool);
    if (status != APEX_OK) return status;
    suggestion = (apex_action_suggestion){sizeof(apex_action_suggestion), 0, str("Hammer"), str("F24"), str("plugin.hello.tool")};
    return actions->register_action(host->context, str("plugin.hello.read_document"), str("Read document"),
                                    str("Log the root class name"), 0, read_document, &suggestion, 1);
}
static const apex_plugin plugin = { sizeof(apex_plugin), APEX_PLUGIN_ABI_MAJOR, APEX_PLUGIN_ABI_MINOR, 0, 0, activate, 0, read_document, 0 };
```

<!-- snippet: examples/hello-tool/hello.c -->
```c
APEX_PLUGIN_EXPORT const apex_plugin* apex_plugin_entry(const apex_host* h) { host = h; return &plugin; }
```

## Crashes and safe mode

Apex keeps a memory-mapped breadcrumb recording which plugin is running and in which callback. If the
previous run died inside a plugin, the next start names it and offers to disable it. `apex --no-plugins`
starts with every plugin disabled. The plugin manager (Settings → Plugins…) lists discovered plugins, their ABI
status, and any load errors, and lets you enable or disable them (takes effect on restart).
