#pragma once
#include <functional>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <cstdint>
#include <apex/apex_plugin.h>
namespace apex::document { class Document; }
namespace apex::editor { class EditorSession; }
namespace apex::plugin_host {
struct Plugin { std::string id, name, version, error; std::filesystem::path folder, entry; bool enabled = true, loaded = false; };
bool inNamespace(std::string_view plugin, std::string_view id);
class UiBackend {
public:
    virtual ~UiBackend() = default;
    // A plugin asked for this panel, section or dialog to be described again.
    virtual void formChanged(std::string_view id) = 0;
    // Runs a modal dialog for the entry `id` (describe it with Host::describePanel). True when accepted.
    virtual bool runDialog(std::string_view id, std::string_view title) { (void)id; (void)title; return false; }
};
// What a host panel passes to a section: kind "materials" (names), "entities" or "sides" (ids).
struct UiContext { std::string kind; std::vector<std::string> names; std::vector<std::uint64_t> ids; };
struct UiSection { std::string id, title; };
class Host {
public:
    Host(std::filesystem::path state, std::filesystem::path breadcrumb,
         std::function<void(std::string_view)> log = {});
    ~Host();
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;
    void discover(const std::vector<std::filesystem::path>& paths);
    void activate(bool safeMode = false);
    void setPanelRegistrar(std::function<bool(std::string, std::string)> registrar);
    void setUiBackend(UiBackend* backend);
    bool describePanel(std::string_view id, const std::function<void(const apex_ui_form&)>& consume);
    bool describePanel(std::string_view id, const UiContext* context, const std::function<void(const apex_ui_form&)>& consume);
    std::vector<UiSection> sections(std::string_view target) const;
    bool hasPanel(std::string_view id) const;
    void panelChanged(std::string_view id, std::string_view key, const apex_ui_value& value);
    void panelClicked(std::string_view id, std::string_view key);
    void setDocument(const document::Document* document);
    void setSession(editor::EditorSession* session);
    void setEnabled(std::string_view id, bool enabled);
    const std::vector<Plugin>& plugins() const;
    std::string staleBreadcrumb() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
