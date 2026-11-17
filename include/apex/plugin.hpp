#pragma once
#include "apex_plugin.h"
#include <string_view>

namespace apex::plugin {
inline apex_str string(std::string_view text) { return {text.data(), text.size()}; }
inline std::string_view view(apex_str text) { return text.data ? std::string_view(text.data, text.size) : std::string_view{}; }

// The wrapper owns no foreign memory. Derived objects must outlive their registration.
class Tool {
public:
    virtual ~Tool() = default;
    virtual void activate() {}
    virtual void deactivate() {}
    virtual bool input(const apex_tool_event&, const apex_tool_view&) { return false; }
    virtual apex_status visual(uint64_t, apex_tool_visual&) { return APEX_OK; }
    virtual apex_status readout(apex_tool_readout&) { return APEX_OK; }
    virtual bool setParameter(std::string_view, double) { return false; }
    virtual bool perform(std::string_view) { return false; }
    virtual void cancel() {}
    virtual void documentChanged() {}
    virtual void forget(uint64_t) {}
    static const apex_tool_callbacks* callbacks() {
        static const apex_tool_callbacks table{
            sizeof(table),
            [](void* p) { try { static_cast<Tool*>(p)->activate(); } catch (...) {} },
            [](void* p) { try { static_cast<Tool*>(p)->deactivate(); } catch (...) {} },
            [](void* p, const apex_tool_event* e, const apex_tool_view* v) -> int {
                try { return e && v && static_cast<Tool*>(p)->input(*e, *v); } catch (...) { return 0; }
            },
            [](void* p, uint64_t s, apex_tool_visual* v) -> apex_status {
                try { return v ? static_cast<Tool*>(p)->visual(s, *v) : APEX_ERR_INVALID; } catch (...) { return APEX_ERR_INTERNAL; }
            },
            [](void* p, apex_tool_readout* r) -> apex_status {
                try { return r ? static_cast<Tool*>(p)->readout(*r) : APEX_ERR_INVALID; } catch (...) { return APEX_ERR_INTERNAL; }
            },
            [](void* p, apex_str id, double n) -> int { try { return static_cast<Tool*>(p)->setParameter(view(id), n); } catch (...) { return 0; } },
            [](void* p, apex_str id) -> int { try { return static_cast<Tool*>(p)->perform(view(id)); } catch (...) { return 0; } },
            [](void* p) { try { static_cast<Tool*>(p)->cancel(); } catch (...) {} },
            [](void* p) { try { static_cast<Tool*>(p)->documentChanged(); } catch (...) {} },
            [](void* p, uint64_t s) { try { static_cast<Tool*>(p)->forget(s); } catch (...) {} }
        };
        return &table;
    }
};
inline const apex_tools_v1* tools(const apex_host& host) {
    return static_cast<const apex_tools_v1*>(host.get_interface(host.context, string("apex.tools/1")));
}
inline const apex_actions_v1* actions(const apex_host& host) {
    return static_cast<const apex_actions_v1*>(host.get_interface(host.context, string("apex.actions/1")));
}
}
