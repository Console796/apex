#pragma once

#include "editor/tool.hpp"
#include "document/entity_helpers.hpp"

namespace apex::editor {
class EditorSession;

struct LightControl {
    std::string key, label;
    double minimum = 0, maximum = 1048576, step = 1;
};
std::optional<LightControl> lightControl(std::string_view key);

// Select-tool handles preview entity keys, then commit once on release. The source VMF and
// history are untouched during a drag; Escape/right-click discard the preview.
class LightEdit {
public:
    bool input(EditorSession& session, Preview& preview, const InputEvent& event, const ToolView& view);
    void overlay(const EditorSession& session, ToolOverlay& overlay, const ToolView& view) const;
    std::vector<ToolParameter> parameters(const EditorSession& session) const;
    bool setParameter(EditorSession& session, std::string_view key, double value);
    bool active() const { return drag_.has_value(); }
    bool owns(InputSource source) const { return drag_ && drag_->source == source; }
    void cancel(Preview& preview) { hovered_.reset(); drag_.reset(); preview.setEntityKeys({}); }
    std::string status() const;
private:
    struct Handle {
        std::string key, label;
        DVec3 point{0}, axis{1, 0, 0};
        double value = 0, distance = 0;
        Vec4 color{1};
    };
    struct Drag {
        InputSource source;
        document::NodeId node;
        Handle handle;
        DVec3 start, planeNormal;
        double value;
        std::map<std::string, std::string> keys;
    };
    std::optional<Handle> hovered_;
    std::optional<Drag> drag_;
    std::vector<Handle> handles(const EditorSession& session) const;
};
} // namespace apex::editor
