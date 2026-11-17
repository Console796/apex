#pragma once
#include "plugin_host/script_engine.hpp"
#include <cstddef>
namespace apex::plugin_host {
// plugins.md D10: the AngelScript implementation of the ScriptEngine seam. The engine is a bundled plugin
// in the sense of D1: it talks to the editor only through the plugin C ABI (apex_host and the interfaces
// behind it), exactly like a native plugin, and exposes those interfaces to the script as the `apex::`
// namespace (docs/plugins/script-host.md).
//
// It is compiled only when the build option APEX_WITH_ANGELSCRIPT is ON (then APEX_HAVE_ANGELSCRIPT is
// defined for everything that links apex_plugin_host). When it is OFF the factory below returns null, so
// a script plugin stays listed with the host's "no engine for this runtime" error and nothing else changes.
constexpr bool angelScriptCompiledIn() {
#if defined(APEX_HAVE_ANGELSCRIPT)
    return true;
#else
    return false;
#endif
}
// A factory that returns an AngelScript engine for the runtime "angelscript" (case sensitive), or null for
// any other runtime or when AngelScript is not compiled in.
ScriptEngineFactory defaultScriptEngineFactory();
// How many AngelScript engines currently exist (loaded, not yet unloaded and destroyed). A diagnostic for
// tests that check a reload does not leak an engine; 0 when AngelScript is not compiled in.
std::size_t liveAngelScriptEngines();
}
