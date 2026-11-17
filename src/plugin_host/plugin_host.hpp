#pragma once
#include <functional>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <cstdint>
#include <chrono>
#include <map>
#include <optional>
#include <apex/apex_plugin.h>
#include "assets/vfs.hpp"
#include "core/math.hpp"
#include "plugin_host/script_engine.hpp"
namespace apex::document { class Document; }
namespace apex::editor { class EditorSession; class ProblemRegistry; }
namespace apex::plugin_host {
struct Plugin {
    std::string id, name, version, error;
    std::filesystem::path folder, entry;
    bool enabled = true, loaded = false;
    // 0.9.7: a script plugin names `script` (a file inside its folder) and `runtime` instead of `entry`.
    std::filesystem::path script;
    std::string runtime;
};
// Ids: a plugin id is dot-separated parts of letters, digits, '_' and '-'; everything it registers is
// "plugin.<id>.<name>" with one such part as the name. inNamespace is true only for a valid id of that shape
// under exactly this plugin, so ids cannot overlap between com.example and com.example.paint.
bool validPluginId(std::string_view id);
bool validItemName(std::string_view name);
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
// apex.io/1: one registered importer.
struct ImporterInfo { std::string id, name, owner; std::vector<std::string> extensions; };
struct ImportResult { bool ok = false; std::string message; };
// apex.assets/1: what a plugin may read and write about the game's materials. The UI implements it over
// its GameContent; every call is on the editor thread.
struct MaterialRecord { std::string name, shader, baseTexture, surfaceProp, source; };
struct VmtEditRequest { std::string key, value; bool remove = false; };
struct AssetsWriteResult { bool ok = false; bool unsupported = false; bool notFound = false; std::string message; };
class AssetsBackend {
public:
    virtual ~AssetsBackend() = default;
    // Calls `visit` for each indexed material until it returns false.
    virtual void materials(const std::function<bool(const MaterialRecord&)>& visit) = 0;
    virtual std::optional<MaterialRecord> material(std::string_view name) = 0;
    // Edits the VMT through Apex's writer and refreshes that material in the index.
    virtual AssetsWriteResult writeVmt(std::string_view material, const std::vector<VmtEditRequest>& edits) = 0;
};
// One registered apex.vfs/1 provider, for the plugin manager and tests.
struct VfsProviderInfo { std::string id, name, owner; bool faulted = false; std::size_t inFlight = 0; };
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
    // ---- apex.io/1 (importers) ----
    // Registered importers in registration order. Extensions are lower case, without dots.
    const std::vector<ImporterInfo>& importers() const;
    // The first importer that handles `path`'s extension, or null.
    const ImporterInfo* importerFor(const std::filesystem::path& path) const;
    // Runs the importer inside ONE edit transaction on the session's document (one undo step; a failure
    // rolls everything back and `message` says why). Needs a session with a document.
    ImportResult importFile(std::string_view importerId, const std::filesystem::path& path);
    // ---- apex.jobs/1 ----
    // Runs the completions of finished jobs on the calling thread, which must be the editor (main) thread.
    // The UI calls this from a timer or event; returns how many completions ran.
    std::size_t drainJobs();
    // Jobs submitted and not yet delivered (thread-safe).
    std::size_t pendingJobs() const;
    // ---- apex.watch/1 ----
    // Paths a plugin may watch besides its own folder (the game directory). Call from the editor thread.
    void addWatchRoot(const std::filesystem::path& root);
    // How long a watched path must be quiet before its change is delivered (default 200 ms).
    void setWatchDebounce(std::chrono::milliseconds quiet);
    // Stats the watched paths and delivers the changes that have settled, on the calling (editor) thread.
    // The UI calls this from a timer a few times a second; tests pass their own clock.
    std::size_t pollWatches(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    bool hasWatches() const;
    // ---- apex.vfs/1 and apex.assets/1 ----
    // Providers registered during activate, in registration order, for Vfs::load. Safe to call from any
    // thread. Each is a thread-safe adapter that bounds every plugin call by the timeout below, catches its
    // exceptions, and takes the plugin out of service after a timeout.
    std::vector<assets::VfsProviderMount> vfsProviders() const;
    std::vector<VfsProviderInfo> vfsProviderInfos() const;
    // The longest a single provider call may take (default 5 s). Applies to calls made after this.
    void setVfsTimeout(std::chrono::milliseconds timeout);
    // Called on the editor thread when a plugin asks for its provider's files to be reloaded.
    void setVfsRefreshHandler(std::function<void(std::string_view providerId)> handler);
    void setAssetsBackend(AssetsBackend* backend);
    void setDocument(const document::Document* document);
    void setSession(editor::EditorSession* session);
    // ---- script plugins (plugins.md D10) ----
    // Supplies engines for `runtime` names. Call before activate; without a factory (or when it returns null)
    // a script plugin stays listed with an error naming the missing runtime.
    void setScriptEngineFactory(ScriptEngineFactory factory);
    // Loads the script again with a new engine, on the editor thread. Done automatically (through an
    // apex.watch/1-style watch on the script file, delivered by pollWatches) when the file is saved.
    // Tool instances of the old version are retired, its callbacks go dead, and registrations of the same id
    // keep their registry entry and are rebound to the new version. An id the new version drops stays as an
    // inert entry; an id it adds is not available until restart once the registries have frozen (logged).
    // When the new version fails to load the script plugin stays unloaded with its error until the next save.
    bool reloadScript(std::string_view id, std::string* message = nullptr);
    void setEnabled(std::string_view id, bool enabled);
    const std::vector<Plugin>& plugins() const;
    std::string staleBreadcrumb() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
