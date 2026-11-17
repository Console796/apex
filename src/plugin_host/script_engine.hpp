#pragma once
#include <apex/apex_plugin.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
namespace apex::plugin_host {
// plugins.md D10: the seam between the plugin host and a scripting language. A script plugin's manifest
// names a `script` file and a `runtime`; the host asks the factory for an engine for that runtime and
// lets it load the file. Nothing here is language specific; the AngelScript engine (not in this tree yet)
// is one implementation, tests use a fake one.
struct ScriptSource {
    std::string pluginId;               // the manifest id; every registration the script makes lands in plugin.<id>.*
    std::filesystem::path folder;       // the plugin's folder
    std::filesystem::path file;         // the `script` entry, inside the folder
    std::string runtime;                // the manifest's `runtime`
};
class ScriptEngine {
public:
    virtual ~ScriptEngine() = default;
    // Compiles and runs the script's registration entry point. While this call runs, the host API reached
    // through `host` registers on behalf of the script plugin (tools, actions, panels, overlays, watches...),
    // exactly as `activate` does for a native plugin. Return false and fill `error` (compiler message with
    // line numbers, ideally) when the script does not compile or its entry point fails; the host surfaces
    // that through the plugin's error, host->last_error and the log, and never lets it escape.
    virtual bool load(const ScriptSource& source, const apex_host* host, std::string& error) = 0;
    // Releases the loaded module. The host calls this, on the editor thread, after it has retired every
    // tool instance and neutralised every callback the script registered, so nothing calls into the module
    // afterwards. A reload loads the new version into a NEW engine, so this is also the final call.
    virtual void unload() = 0;
    // Called on the editor thread after every document change, while the script is loaded.
    virtual void documentChanged(const apex_change_notice&) {}
};
// Returns an engine for `runtime` (for example "angelscript"), or null when this build has none.
using ScriptEngineFactory = std::function<std::unique_ptr<ScriptEngine>(std::string_view runtime)>;
}
