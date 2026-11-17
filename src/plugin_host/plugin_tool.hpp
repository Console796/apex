#pragma once
#include "editor/tool.hpp"
#include <apex/apex_plugin.h>
#include <functional>
#include <map>
#include <set>

namespace apex::plugin_host {
// The guard runs every foreign callback under the host's crash breadcrumb.
class PluginTool final : public editor::Tool {
public:
    using Guard = std::function<void(const char*, const std::function<void()>&)>;
    PluginTool(editor::EditorSession& session, std::string id, void* user,
               apex_tool_callbacks callbacks, Guard guard, std::function<void(const char*)> error = {});
    ~PluginTool() override;
    std::weak_ptr<std::function<void()>> retirement() const { return retirement_; }
    // Script reload (0.9.7): a retired tool can be pointed at the new version's callbacks. It gets a fresh
    // instance and, when it was the active tool, its activate callback runs again.
    using Revive = std::function<void(void* registrationUser, const apex_tool_callbacks&)>;
    std::weak_ptr<Revive> reviver() const { return reviver_; }
    void retire();
    // True when the tool runs on the registration's user pointer (no create_instance, or it returned null).
    bool sharesRegistrationState() const { return !ownsUser_; }
    editor::ToolId id() const override { return id_; }
    void activate() override;
    void deactivate() override;
    editor::Handled input(const editor::InputEvent&, const editor::ToolView&) override;
    const editor::Preview& preview() const override { return preview_; }
    editor::ToolOverlay overlay(const editor::ToolView&, editor::InputSource source) const override;
    editor::ToolReadout readout() const override;
    void cancel() override;
    bool owns(editor::InputSource source) const override;
    void forget(editor::InputSource source) override;
    void documentChanged() override;
    bool setParameter(std::string_view id, double value) override;
    bool perform(const editor::ActionId& action) override;
private:
    void refresh(editor::InputSource source);
    void refreshAll();
    void rebuildPreview();
    editor::EditorSession& session_;
    std::string id_;
    void* user_;
    bool ownsUser_ = false;
    apex_tool_callbacks callbacks_;
    Guard guard_;
    std::function<void(const char*)> error_;
    std::shared_ptr<std::function<void()>> retirement_;
    std::shared_ptr<Revive> reviver_;
    bool active_ = false;
    void createInstance(void* registrationUser);
    editor::Preview preview_;
    editor::InputSource lastSource_ = 0;
    // Per pane (input source) state: the ghosts its visual callback last returned, and the panes with a
    // pointer gesture this tool handled the press of. The preview shows every pane's ghosts.
    std::map<editor::InputSource, std::vector<editor::GhostSolid>> ghosts_;
    std::set<editor::InputSource> pressed_;
};
}
