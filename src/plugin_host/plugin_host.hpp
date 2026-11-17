#pragma once
#include <functional>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <cstdint>
#include <map>
#include <apex/apex_plugin.h>
#include "core/math.hpp"
namespace apex::document { class Document; }
namespace apex::editor { class EditorSession; class ProblemRegistry; }
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
// apex.overlay/1 and apex.helpers/1: world primitives a plugin drew, copied by the host. Qt-free; the
// viewport pane turns them into draw-list entries.
struct OverlaySegment { DVec3 a{0.0}, b{0.0}; Vec4 colour{1.0f}; float width = 2.0f; };
struct OverlayPoint { DVec3 position{0.0}; Vec4 colour{1.0f}; float size = 7.0f; };
struct OverlayTriangle { DVec3 a{0.0}, b{0.0}, c{0.0}; Vec4 colour{1.0f}; };
struct OverlayLabel { DVec3 position{0.0}; std::string text; Vec4 colour{1.0f}; };
// One declared plugin overlay. A ghost is stored as fan triangles in `triangles` plus its edges in `segments`.
struct OverlayData {
    std::string id, name;
    std::vector<OverlaySegment> segments;
    std::vector<OverlayPoint> points;
    std::vector<OverlayTriangle> triangles;
    std::vector<OverlayLabel> labels;
    std::map<std::int32_t, Vec4> tints; // side ID -> RGBA
};
struct HelperDrawing {
    std::vector<OverlaySegment> segments;
    std::vector<OverlayLabel> labels;
};
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
    // Plugin problem checks register here (default: editor::mutableProblemRegistry()). Call before activate.
    void useProblemRegistry(editor::ProblemRegistry& registry);
    // Declared overlays in declaration order, whether or not they currently hold data.
    const std::vector<OverlayData>& overlays() const;
    // Bumps whenever any overlay list changes or is dropped; panes compare it to know to redraw.
    std::uint64_t overlayRevision() const;
    // What plugins' helpers draw for the current document, recomputed when the document or the class table
    // changed. Empty without helper plugins, a document or a session.
    const HelperDrawing& helperDrawing();
    bool hasHelpers() const;
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
