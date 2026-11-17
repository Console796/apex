#pragma once
#include "editor/tool.hpp"
#include <apex/apex_plugin.h>
#include <functional>

namespace apex::plugin_host {
// The guard runs every foreign callback under the host's crash breadcrumb.
class PluginTool final : public editor::Tool {
public:
    using Guard = std::function<void(const char*, const std::function<void()>&)>;
    PluginTool(editor::EditorSession& session, std::string id, void* user,
               apex_tool_callbacks callbacks, Guard guard, std::function<void(const char*)> error = {});
    ~PluginTool() override;
    std::weak_ptr<std::function<void()>> retirement() const { return retirement_; }
    void retire();
    editor::ToolId id() const override { return id_; }
    void activate() override;
    void deactivate() override;
    editor::Handled input(const editor::InputEvent&, const editor::ToolView&) override;
    const editor::Preview& preview() const override { return preview_; }
    editor::ToolOverlay overlay(const editor::ToolView&, editor::InputSource source) const override;
    editor::ToolReadout readout() const override;
    void cancel() override;
    void forget(editor::InputSource source) override;
    void documentChanged() override;
    bool setParameter(std::string_view id, double value) override;
    bool perform(const editor::ActionId& action) override;
private:
    void refresh(editor::InputSource source);
    editor::EditorSession& session_;
    std::string id_;
    void* user_;
    bool ownsUser_ = false;
    apex_tool_callbacks callbacks_;
    Guard guard_;
    std::function<void(const char*)> error_;
    std::shared_ptr<std::function<void()>> retirement_;
    editor::Preview preview_;
    editor::InputSource lastSource_ = 0;
};
}
