#include "plugin_host/plugin_host.hpp"
#include "plugin_host/plugin_tool.hpp"
#include "core/kv3.hpp"
#include "core/log.hpp"
#include "document/document.hpp"
#include "editor/editor_session.hpp"
#include "geometry/solid_block.hpp"
#include <apex/apex_plugin.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <utility>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace apex::plugin_host {
namespace {
constexpr std::size_t kBreadcrumbSize = 4096;
class Breadcrumb {
public:
    explicit Breadcrumb(const std::filesystem::path& path) {
        if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
#ifdef _WIN32
        file_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot open breadcrumb");
        mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READWRITE, 0, kBreadcrumbSize, nullptr);
        if (mapping_) data_ = static_cast<char*>(MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, kBreadcrumbSize));
#else
        fd_ = open(path.c_str(), O_RDWR | O_CREAT, 0600);
        if (fd_ < 0 || ftruncate(fd_, kBreadcrumbSize) != 0) throw std::runtime_error("cannot open breadcrumb");
        void* data = mmap(nullptr, kBreadcrumbSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (data != MAP_FAILED) data_ = static_cast<char*>(data);
#endif
        if (!data_) throw std::runtime_error("cannot map breadcrumb");
        stale_ = std::string(data_, std::find(data_, data_ + kBreadcrumbSize, '\0'));
        clear();
    }
    ~Breadcrumb() {
#ifdef _WIN32
        if (data_) UnmapViewOfFile(data_);
        if (mapping_) CloseHandle(mapping_);
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
#else
        if (data_) munmap(data_, kBreadcrumbSize);
        if (fd_ >= 0) close(fd_);
#endif
    }
    void set(std::string_view id, std::string_view callback) {
        const auto text = std::string(id) + ":" + std::string(callback);
        const auto size = std::min(text.size(), kBreadcrumbSize - 1);
        std::memcpy(data_, text.data(), size);
        data_[size] = '\0';
    }
    void clear() { data_[0] = '\0'; }
    std::string stale() const { return stale_; }
private:
    char* data_ = nullptr;
    std::string stale_;
#ifdef _WIN32
    HANDLE file_ = INVALID_HANDLE_VALUE, mapping_ = nullptr;
#else
    int fd_ = -1;
#endif
};
struct Mark {
    Breadcrumb& breadcrumb;
    Mark(Breadcrumb& b, std::string_view id, std::string_view callback) : breadcrumb(b) { b.set(id, callback); }
    ~Mark() { breadcrumb.clear(); }
};
std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read " + path.string());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
std::string field(const kv3::Value& object, std::string_view name) {
    const auto* value = object.find(name);
    const auto* text = value ? value->get<std::string>() : nullptr;
    return text ? *text : std::string{};
}
std::string platform() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    return "windows-x86_64";
#elif defined(__linux__) && defined(__x86_64__)
    return "linux-x86_64";
#else
    return "unsupported";
#endif
}
std::string_view view(apex_str s) { return s.data ? std::string_view(s.data, s.size) : std::string_view{}; }
apex_str string(std::string_view s) { return {s.data(), s.size()}; }
#ifdef _WIN32
using Library = HMODULE;
Library openLibrary(const std::filesystem::path& path) { return LoadLibraryW(path.c_str()); }
void closeLibrary(Library lib) { if (lib) FreeLibrary(lib); }
apex_plugin_entry_fn entryPoint(Library lib) { return reinterpret_cast<apex_plugin_entry_fn>(GetProcAddress(lib, "apex_plugin_entry")); }
#else
using Library = void*;
Library openLibrary(const std::filesystem::path& path) { return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL); }
void closeLibrary(Library lib) { if (lib) dlclose(lib); }
apex_plugin_entry_fn entryPoint(Library lib) { return reinterpret_cast<apex_plugin_entry_fn>(dlsym(lib, "apex_plugin_entry")); }
#endif
}

bool inNamespace(std::string_view plugin, std::string_view id) {
    const auto prefix = "plugin." + std::string(plugin) + ".";
    return id.starts_with(prefix) && id.size() > prefix.size();
}

namespace {
class RegisteredTool final : public editor::Tool {
public:
    RegisteredTool(std::string id, std::function<void()> activate, std::function<void()> deactivate)
        : id_(std::move(id)), activate_(std::move(activate)), deactivate_(std::move(deactivate)) {}
    editor::ToolId id() const override { return id_; }
    void activate() override { if (activate_) activate_(); }
    void deactivate() override { if (deactivate_) deactivate_(); }
    editor::Handled input(const editor::InputEvent&, const editor::ToolView&) override { return false; }
    const editor::Preview& preview() const override { return preview_; }
    editor::ToolOverlay overlay(const editor::ToolView&, editor::InputSource) const override { return {}; }
    editor::ToolReadout readout() const override { return {}; }
    void cancel() override {}
private:
    std::string id_;
    std::function<void()> activate_, deactivate_;
    editor::Preview preview_;
};
}

struct Host::Impl {
    struct Loaded { std::size_t index; Library library; const apex_plugin* plugin; };
    std::filesystem::path state;
    Breadcrumb breadcrumb;
    std::function<void(std::string_view)> logger;
    std::vector<Plugin> plugins;
    std::vector<Loaded> loaded;
    // All editor callbacks run on the editor thread. Invalidate before unloading code.
    std::shared_ptr<bool> lifetime = std::make_shared<bool>(true);
    std::vector<std::weak_ptr<std::function<void()>>> toolRetirements;
    std::set<std::string> disabled;
    const document::Document* document = nullptr;
    editor::EditorSession* session = nullptr;
    document::Document* observedDocument = nullptr;
    std::size_t observerToken = 0;
    std::string scratch, lastError;
    apex_host host{};
    apex_document_v1 documentApi{};
    apex_registration_v1 registrationApi{};
    apex_tools_v1 toolsApi{};
    apex_actions_v1 actionsApi{};
    apex_edit_v1 editApi{};
    apex_selection_v1 selectionApi{};
    apex_ui_v1 uiApi{};
    UiBackend* uiBackend = nullptr;
    std::string registering;
    std::string callbackOwner;
    std::function<bool(std::string, std::string)> panelRegistrar;
    struct Registration {
        enum class Kind { Action, Tool, Panel, Section, Dialog } kind;
        std::string id, name, description, owner, target;
        void* user;
        apex_callback first, second;
        bool extended = false;
        apex_tool_callbacks callbacks{};
        apex_ui_callbacks uiCallbacks{};
        std::map<std::string, std::string, std::less<>> suggestions, suggestionContexts;
    };
    std::vector<Registration> pending;
    std::map<std::string, Registration, std::less<>> uiPanels;  // panels, sections and open dialogs
    unsigned dialogCounter = 0;
    Impl(std::filesystem::path s, std::filesystem::path b, std::function<void(std::string_view)> log)
        : state(std::move(s)), breadcrumb(b), logger(std::move(log)) {
        lastError.reserve(512);
        std::ifstream file(state);
        for (std::string line; std::getline(file, line);) if (!line.empty()) disabled.insert(line);
        host = {sizeof(host), APEX_PLUGIN_ABI_MAJOR, APEX_PLUGIN_ABI_MINOR, this, interface, writeLog, error};
        documentApi = {sizeof(documentApi), info, rootAt, node, childAt, keyAt};
        registrationApi = {sizeof(registrationApi), registerAction, registerTool, registerPanel};
        toolsApi = {sizeof(toolsApi), registerToolV1};
        actionsApi = {sizeof(actionsApi), registerActionV1};
        editApi = {sizeof(editApi), applyEdit, deferEdit};
        selectionApi = {sizeof(selectionApi), selectionInfo, selectionAt, selectionSet};
        uiApi = {sizeof(uiApi), registerUiPanel, refreshUi, registerUiSection, showUiDialog};
    }
    void setError(const char* message) noexcept {
        try { lastError.assign(message, std::min(std::strlen(message), lastError.capacity())); } catch (...) { }
    }
    static apex_status internal(void* c) noexcept {
        try { throw; }
        catch (const std::exception& e) { self(c).setError(e.what()); }
        catch (...) { self(c).setError("exception in host API"); }
        return APEX_ERR_INTERNAL;
    }
    static Impl& self(void* context) { return *static_cast<Impl*>(context); }
    static const void* interface(void* context, apex_str name) noexcept {
        if (view(name) == "apex.document/1") return &self(context).documentApi;
        if (view(name) == "apex.registration/1") return &self(context).registrationApi;
        if (view(name) == "apex.tools/1") return &self(context).toolsApi;
        if (view(name) == "apex.actions/1") return &self(context).actionsApi;
        if (view(name) == "apex.edit/1") return &self(context).editApi;
        if (view(name) == "apex.selection/1") return &self(context).selectionApi;
        if (view(name) == "apex.ui/1") return &self(context).uiApi;
        return nullptr;
    }
    static apex_status selectionInfo(void* c, uint32_t* mode, size_t* count) try {
        const auto* session = self(c).session;
        if (!session || !session->document()) return APEX_ERR_STATE;
        if (!mode || !count) return APEX_ERR_INVALID;
        *mode = static_cast<uint32_t>(session->selection().mode());
        *count = session->selection().ordered().size();
        return APEX_OK;
    } catch (...) { return internal(c); }
    static apex_status selectionAt(void* c, size_t index, uint64_t* id) try {
        const auto* session = self(c).session;
        if (!session || !session->document()) return APEX_ERR_STATE;
        if (!id) return APEX_ERR_INVALID;
        const auto& targets = session->selection().ordered();
        if (index >= targets.size()) return APEX_ERR_NOT_FOUND;
        *id = std::visit([](auto value) { return static_cast<uint64_t>(value); }, targets[index]);
        return APEX_OK;
    } catch (...) { return internal(c); }
    static apex_status selectionSet(void* c, uint32_t mode, const uint64_t* ids, size_t count) try {
        auto* session = self(c).session;
        if (!session || !session->document()) return APEX_ERR_STATE;
        if (mode > static_cast<uint32_t>(editor::SelectionMode::Faces) || (count && !ids)) return APEX_ERR_INVALID;
        editor::Selection next(static_cast<editor::SelectionMode>(mode));
        for (size_t i = 0; i < count; ++i) {
            if (mode == static_cast<uint32_t>(editor::SelectionMode::Faces)) {
                const auto side = static_cast<document::SideId>(ids[i]);
                if (!session->document()->side(side)) return APEX_ERR_NOT_FOUND;
                next.add(editor::Target{side});
            } else {
                const auto node = static_cast<document::NodeId>(ids[i]);
                if (!session->document()->handle(node)) return APEX_ERR_NOT_FOUND;
                next.add(editor::Target{node});
            }
        }
        session->selection() = next;
        return APEX_OK;
    } catch (...) { return internal(c); }
    struct EditContext {
        document::Transaction& tx;
        document::Document& doc;
        Impl& host;
        apex_status failure = APEX_OK;
    };
    template<class F> static apex_status editOperation(EditContext& context, F&& operation) noexcept {
        if (context.failure != APEX_OK) return context.failure;
        try { operation(); return APEX_OK; }
        catch (const std::invalid_argument& e) {
            context.host.setError(e.what());
            context.failure = APEX_ERR_INVALID;
        }
        catch (...) { context.failure = internal(&context.host); }
        return context.failure;
    }
    static apex_status setKey(void* c, apex_node_id id, apex_str key, apex_str value) {
        auto& e = *static_cast<EditContext*>(c);
        return editOperation(e, [&] {
            const auto h = e.doc.handle(id);
            if (!h) throw std::invalid_argument("unknown node ID");
            e.tx.setKeyValue(*h, std::string(view(key)), std::string(view(value)));
        });
    }
    static apex_status destroyNode(void* c, apex_node_id id) {
        auto& e = *static_cast<EditContext*>(c);
        return editOperation(e, [&] {
            const auto h = e.doc.handle(id);
            if (!h) throw std::invalid_argument("unknown node ID");
            e.tx.destroy(*h);
        });
    }
    static apex_status createSolid(void* c, apex_node_id parent, const apex_plane* values, size_t count, apex_str material) {
        auto& e = *static_cast<EditContext*>(c);
        return editOperation(e, [&] {
        if (!values || count < 4 || count > 256) throw std::invalid_argument("invalid plane count");
        const auto h = e.doc.handle(parent);
        if (!h) throw std::invalid_argument("unknown parent ID");
        std::vector<geometry::PlaneSide> sides;
        for (size_t i = 0; i < count; ++i) {
            geometry::PlaneSide side;
            side.plane = {{values[i].normal.x, values[i].normal.y, values[i].normal.z}, values[i].distance};
            sides.push_back(side);
        }
        auto poly = geometry::ConvexPolyhedron::fromPlanes(sides);
        if (!poly) throw std::invalid_argument("invalid solid planes");
        geometry::NewSolidStyle style;
        style.material = std::string(view(material));
        e.tx.create(geometry::solidBlock(*poly, style), *h);
        });
    }
    static apex_status reparentNode(void* c, apex_node_id id, apex_node_id parent, size_t position) {
        auto& e = *static_cast<EditContext*>(c);
        return editOperation(e, [&] {
            const auto h = e.doc.handle(id);
            const auto p = parent ? e.doc.handle(parent) : std::optional<document::NodeHandle>{};
            if (!h || (parent && !p)) throw std::invalid_argument("unknown node ID");
            e.tx.reparent(*h, p, position);
        });
    }
    static apex_status setSideKey(void* c, int32_t side, apex_str key, apex_str value) {
        auto& e = *static_cast<EditContext*>(c);
        return editOperation(e, [&] { e.tx.setSideKeyValue(side, std::string(view(key)), std::string(view(value))); });
    }
    static apex_status removeDisplacement(void* c, int32_t side) {
        auto& e = *static_cast<EditContext*>(c);
        return editOperation(e, [&] { e.tx.removeDisplacement(side); });
    }
    static apex_status rebuildSolid(void* c, apex_node_id id, const apex_plane* values, size_t count) {
        auto& e = *static_cast<EditContext*>(c);
        return editOperation(e, [&] {
            if (!values || count < 4 || count > 256) throw std::invalid_argument("invalid plane count");
            const auto h = e.doc.handle(id);
            if (!h || !std::holds_alternative<Handle<document::Solid>>(*h)) throw std::invalid_argument("unknown solid ID");
            std::vector<geometry::PlaneSide> sides;
            for (size_t i = 0; i < count; ++i) {
                geometry::PlaneSide side;
                side.plane = {{values[i].normal.x, values[i].normal.y, values[i].normal.z}, values[i].distance};
                sides.push_back(side);
            }
            auto poly = geometry::ConvexPolyhedron::fromPlanes(sides);
            if (!poly) throw std::invalid_argument("invalid solid planes");
            e.tx.rebuildSolid(std::get<Handle<document::Solid>>(*h), *poly);
        });
    }
    static apex_status applyEdit(void* c, apex_str label, apex_str coalesce, void* user, apex_edit_callback callback) try {
        auto& s = self(c);
        if (!s.session || !s.session->document() || !callback) return APEX_ERR_STATE;
        static const apex_edit_ops_v1 ops{sizeof(ops), setKey, destroyNode, createSolid,
                                           reparentNode, setSideKey, removeDisplacement, rebuildSolid};
        apex_status failure = APEX_OK;
        try {
            s.session->edit(std::string(view(label)), [&](document::Transaction& tx) {
                EditContext context{tx, *s.session->document(), s};
                const auto status = callback(user, &ops, &context);
                failure = context.failure;
                if (failure != APEX_OK) throw std::runtime_error("plugin edit operation failed");
                if (status != APEX_OK) throw std::invalid_argument("plugin edit failed");
            }, std::string(view(coalesce)));
            return APEX_OK;
        } catch (const std::invalid_argument& e) { s.setError(e.what()); return APEX_ERR_INVALID; }
        catch (...) { return failure != APEX_OK ? failure : internal(c); }
    } catch (...) { return internal(c); }
    static apex_status deferEdit(void* c, void* user, void (*callback)(void*)) try {
        auto& s = self(c);
        if (!s.session || !s.session->document()) return APEX_ERR_STATE;
        if (!callback) return APEX_ERR_INVALID;
        const auto owner = s.callbackOwner;
        s.session->document()->defer([&s, life = std::weak_ptr(s.lifetime), user, callback, owner] {
            if (life.expired()) return;
            Mark mark(s.breadcrumb, owner, "deferred");
            const auto previous = std::exchange(s.callbackOwner, owner);
            try { callback(user); } catch (...) { s.lastError = "exception in deferred callback"; }
            s.callbackOwner = previous;
        });
        return APEX_OK;
    } catch (...) { return internal(c); }
    void changed(const document::ChangeSet& changes) {
        std::vector<apex_modified_node> modified;
        modified.reserve(changes.modifiedIds.size());
        for (const auto& item : changes.modifiedIds)
            modified.push_back({item.id, static_cast<uint32_t>(item.flags)});
        std::vector<int32_t> sides(changes.vanishedSides.begin(), changes.vanishedSides.end());
        apex_change_notice notice{sizeof(notice), observedDocument->historyState(), changes.createdIds.data(),
            changes.createdIds.size(), modified.data(), modified.size(), changes.destroyedIds.data(),
            changes.destroyedIds.size(), sides.data(), sides.size(), changes.undoOrRedo ? 1u : 0u};
        for (const auto& item : loaded) {
            if (item.plugin->struct_size < offsetof(apex_plugin, on_changed) + sizeof(item.plugin->on_changed) || !item.plugin->on_changed) continue;
            Mark mark(breadcrumb, plugins[item.index].id, "on_changed");
            const auto previous = std::exchange(callbackOwner, plugins[item.index].id);
            try { item.plugin->on_changed(item.plugin->user, &notice); }
            catch (...) { plugins[item.index].error = "exception in on_changed"; }
            callbackOwner = previous;
        }
    }
    static apex_status registerItem(void* context, Registration::Kind kind, apex_str id, apex_str name,
                                    apex_str description, void* user, apex_callback first, apex_callback second) try {
        auto& s = self(context);
        if (s.registering.empty() || editor::actionRegistry().frozen() || editor::toolRegistry().frozen())
            return APEX_ERR_STATE;
        const auto idText = view(id);
        if (!inNamespace(s.registering, idText) || view(name).empty()) {
            s.lastError = "registration outside plugin namespace or missing name";
            return APEX_ERR_INVALID;
        }
        if (std::ranges::any_of(s.pending, [&](const auto& p) { return p.id == idText; }) ||
            s.uiPanels.contains(idText) ||
            ((kind == Registration::Kind::Action || kind == Registration::Kind::Tool) && editor::actionRegistry().find(idText)) ||
            (kind == Registration::Kind::Tool && std::ranges::any_of(editor::toolRegistry().entries(),
                                                     [&](const auto& tool) { return tool.id == idText; }))) {
            s.lastError = "duplicate registration";
            return APEX_ERR_INVALID;
        }
        if (kind == Registration::Kind::Action && !first) return APEX_ERR_INVALID;
        if (kind == Registration::Kind::Panel && !s.panelRegistrar) return APEX_ERR_UNSUPPORTED;
        s.pending.push_back({kind, std::string(idText), std::string(view(name)), std::string(view(description)),
                             s.registering, {}, user, first, second});
        return APEX_OK;
    } catch (...) { return internal(context); }
    static apex_status registerAction(void* c, apex_str id, apex_str name, apex_str desc, void* user, apex_callback fn) try {
        return registerItem(c, Registration::Kind::Action, id, name, desc, user, fn, nullptr);
    } catch (...) { return internal(c); }
    static apex_status registerTool(void* c, apex_str id, apex_str name, apex_str desc, void* user, apex_callback on, apex_callback off) try {
        return registerItem(c, Registration::Kind::Tool, id, name, desc, user, on, off);
    } catch (...) { return internal(c); }
    static apex_status registerActionV1(void* c, apex_str id, apex_str name, apex_str desc, void* user, apex_callback fn,
                                        const apex_action_suggestion* suggestions, size_t count) {
        const auto before = self(c).pending.size();
        try {
            if (count > 64 || (count && !suggestions)) return APEX_ERR_INVALID;
            const auto result = registerItem(c, Registration::Kind::Action, id, name, desc, user, fn, nullptr);
            if (result != APEX_OK) return result;
            auto& item = self(c).pending.back();
            for (size_t i = 0; i < count; ++i) {
                if (view(suggestions[i].preset).empty() || !editor::KeyChord::parse(view(suggestions[i].chord))) {
                    self(c).pending.pop_back(); return APEX_ERR_INVALID;
                }
                item.suggestions.emplace(std::string(view(suggestions[i].preset)), std::string(view(suggestions[i].chord)));
                if (!view(suggestions[i].tool_id).empty())
                    item.suggestionContexts.emplace(std::string(view(suggestions[i].preset)), std::string(view(suggestions[i].tool_id)));
            }
            return APEX_OK;
        } catch (...) { self(c).pending.resize(before); return internal(c); }
    }
    static apex_status registerToolV1(void* c, apex_str id, apex_str name, apex_str desc, void* user,
                                      const apex_tool_callbacks* callbacks) try {
        if (!callbacks || callbacks->struct_size < offsetof(apex_tool_callbacks, input) + sizeof(callbacks->input)) return APEX_ERR_INVALID;
        const auto result = registerItem(c, Registration::Kind::Tool, id, name, desc, user, nullptr, nullptr);
        if (result != APEX_OK) return result;
        auto& item = self(c).pending.back(); item.extended = true;
        item.callbacks.struct_size = sizeof(item.callbacks);
#define APEX_COPY_TOOL_CALLBACK(field) \
        if (callbacks->struct_size >= offsetof(apex_tool_callbacks, field) + sizeof(callbacks->field)) \
            item.callbacks.field = callbacks->field
        APEX_COPY_TOOL_CALLBACK(activate); APEX_COPY_TOOL_CALLBACK(deactivate);
        APEX_COPY_TOOL_CALLBACK(input); APEX_COPY_TOOL_CALLBACK(visual); APEX_COPY_TOOL_CALLBACK(readout);
        APEX_COPY_TOOL_CALLBACK(set_parameter); APEX_COPY_TOOL_CALLBACK(perform); APEX_COPY_TOOL_CALLBACK(cancel);
        APEX_COPY_TOOL_CALLBACK(document_changed); APEX_COPY_TOOL_CALLBACK(forget);
        APEX_COPY_TOOL_CALLBACK(create_instance); APEX_COPY_TOOL_CALLBACK(destroy_instance);
#undef APEX_COPY_TOOL_CALLBACK
        return APEX_OK;
    } catch (...) { return internal(c); }
    static apex_status registerPanel(void* c, apex_str id, apex_str title) try {
        return registerItem(c, Registration::Kind::Panel, id, title, {}, nullptr, nullptr, nullptr);
    } catch (...) { return internal(c); }
    static bool copyUiCallbacks(const apex_ui_callbacks* in, apex_ui_callbacks& out) {
        if (!in || in->struct_size < offsetof(apex_ui_callbacks, describe) + sizeof(in->describe) || !in->describe) return false;
        out = {};
        out.struct_size = sizeof(out);
        out.describe = in->describe;
        if (in->struct_size >= offsetof(apex_ui_callbacks, on_change) + sizeof(in->on_change)) out.on_change = in->on_change;
        if (in->struct_size >= offsetof(apex_ui_callbacks, on_click) + sizeof(in->on_click)) out.on_click = in->on_click;
        return true;
    }
    static apex_status registerUiPanel(void* c, apex_str id, apex_str title, void* user,
                                       const apex_ui_callbacks* callbacks) try {
        apex_ui_callbacks copy;
        if (!copyUiCallbacks(callbacks, copy)) return APEX_ERR_INVALID;
        const auto result = registerItem(c, Registration::Kind::Panel, id, title, {}, user, nullptr, nullptr);
        if (result != APEX_OK) return result;
        self(c).pending.back().uiCallbacks = copy;
        return APEX_OK;
    } catch (...) { return internal(c); }
    static apex_status registerUiSection(void* c, apex_str id, apex_str title, apex_str target, void* user,
                                         const apex_ui_callbacks* callbacks) try {
        apex_ui_callbacks copy;
        const auto where = view(target);
        if (!copyUiCallbacks(callbacks, copy) || (where != "materials" && where != "inspector" && where != "face_properties"))
            return APEX_ERR_INVALID;
        const auto result = registerItem(c, Registration::Kind::Section, id, title, {}, user, nullptr, nullptr);
        if (result != APEX_OK) return result;
        auto& item = self(c).pending.back();
        item.uiCallbacks = copy;
        item.target = std::string(where);
        return APEX_OK;
    } catch (...) { return internal(c); }
    static apex_status refreshUi(void* c, apex_str id) try {
        auto& s = self(c);
        const auto found = s.uiPanels.find(view(id));
        if (found == s.uiPanels.end()) return APEX_ERR_NOT_FOUND;
        if (!s.callbackOwner.empty() && s.callbackOwner != found->second.owner) return APEX_ERR_INVALID;
        if (s.uiBackend) s.uiBackend->formChanged(found->first);
        return APEX_OK;
    } catch (...) { return internal(c); }
    static apex_status showUiDialog(void* c, apex_str title, void* user, const apex_ui_callbacks* callbacks, uint32_t* accepted) try {
        auto& s = self(c);
        apex_ui_callbacks copy;
        if (!accepted || !copyUiCallbacks(callbacks, copy)) return APEX_ERR_INVALID;
        *accepted = 0;
        if (!s.uiBackend) return APEX_ERR_UNSUPPORTED;
        if (s.callbackOwner.empty() && s.registering.empty()) return APEX_ERR_STATE;
        Registration entry{Registration::Kind::Dialog, "dialog." + std::to_string(++s.dialogCounter), std::string(view(title)), {},
                           s.callbackOwner.empty() ? s.registering : s.callbackOwner, {}, user, nullptr, nullptr};
        entry.uiCallbacks = copy;
        const auto key = entry.id;
        s.uiPanels.emplace(key, std::move(entry));
        bool ok = false;
        try { ok = s.uiBackend->runDialog(key, std::string(view(title))); }
        catch (...) { s.uiPanels.erase(key); throw; }
        s.uiPanels.erase(key);
        *accepted = ok ? 1u : 0u;
        return APEX_OK;
    } catch (...) { return internal(c); }
    // Runs one plugin UI callback under the owner's breadcrumb; exceptions never escape.
    template <class Fn> bool guardedUi(const std::string& owner, const char* name, Fn&& fn) noexcept {
        try {
            Mark mark(breadcrumb, owner, name);
            const auto previous = std::exchange(callbackOwner, owner);
            bool ok = true;
            try { fn(); } catch (...) { setError("exception in plugin ui callback"); ok = false; }
            callbackOwner = previous;
            return ok;
        } catch (...) { return false; }
    }
    void callback(const std::string& owner, const char* name, apex_callback fn, void* user) {
        if (!fn) return;
        Mark mark(breadcrumb, owner, name);
        const auto previous = std::exchange(callbackOwner, owner);
        try { fn(user); } catch (...) { lastError = "exception in plugin callback"; }
        callbackOwner = previous;
    }
    void commitRegistrations() {
        for (const auto& r : pending) {
            if (r.kind == Registration::Kind::Section) { uiPanels.emplace(r.id, r); continue; }
            if (r.kind == Registration::Kind::Panel) {
                if (panelRegistrar(r.id, r.name) && r.uiCallbacks.describe) uiPanels.emplace(r.id, r);
                continue;
            }
            const auto callbackFn = [this, life = std::weak_ptr(lifetime), owner = r.owner, user = r.user, fn = r.first] { if (!life.expired()) callback(owner, "action", fn, user); };
            editor::Action action{r.id, r.name, "Plugin", r.description};
            action.suggestedBindings = r.suggestions;
            action.suggestedContexts = r.suggestionContexts;
            editor::mutableActionRegistry().add(std::move(action),
                                                  r.kind == Registration::Kind::Action ? callbackFn : std::function<void()>{});
            if (r.kind == Registration::Kind::Tool) {
                auto* selfPtr = this;
                editor::mutableToolRegistry().add(r.id, [selfPtr, life = std::weak_ptr(lifetime), r](editor::EditorSession& session) -> std::unique_ptr<editor::Tool> {
                    if (life.expired()) return std::make_unique<RegisteredTool>(r.id, nullptr, nullptr);
                    if (r.extended) {
                        auto guard = [selfPtr, life, owner = r.owner](const char* name, const std::function<void()>& fn) {
                            if (life.expired()) return;
                            Mark mark(selfPtr->breadcrumb, owner, name);
                            const auto previous = std::exchange(selfPtr->callbackOwner, owner);
                            try { fn(); } catch (...) { selfPtr->lastError = "exception in plugin tool callback"; }
                            selfPtr->callbackOwner = previous;
                        };
                        auto tool = std::make_unique<PluginTool>(session, r.id, r.user, r.callbacks, std::move(guard),
                            [selfPtr, life](const char* message) { if (!life.expired()) selfPtr->setError(message); });
                        selfPtr->toolRetirements.push_back(tool->retirement());
                        return tool;
                    }
                    auto on = [selfPtr, life, r] { if (!life.expired()) selfPtr->callback(r.owner, "tool.activate", r.first, r.user); };
                    auto off = [selfPtr, life, r] { if (!life.expired()) selfPtr->callback(r.owner, "tool.deactivate", r.second, r.user); };
                    return std::make_unique<RegisteredTool>(r.id, std::move(on), std::move(off));
                });
            }
        }
        pending.clear();
    }
    static void writeLog(void* context, int, apex_str message) try {
        auto& s = self(context);
        if (s.logger) s.logger(view(message)); else log::info("plugin: {}", view(message));
    } catch (...) { internal(context); }
    static apex_str error(void* context) noexcept { return string(self(context).lastError); }
    static const document::Node* find(Impl& s, apex_node_id id) {
        if (!s.document) return nullptr;
        auto handle = s.document->handle(id);
        return handle ? s.document->get(*handle) : nullptr;
    }
    static apex_status info(void* context, apex_document_info* out) try {
        if (!out || out->struct_size < sizeof(*out)) return APEX_ERR_INVALID;
        const auto* doc = self(context).document;
        out->is_open = doc != nullptr;
        out->revision = doc ? doc->historyState() : 0;
        out->root_count = doc ? doc->roots().size() : 0;
        return APEX_OK;
    } catch (...) { return internal(context); }
    static apex_status rootAt(void* context, size_t index, apex_node_id* out) try {
        const auto* doc = self(context).document;
        if (!out) return APEX_ERR_INVALID;
        if (!doc) return APEX_ERR_STATE;
        if (index >= doc->roots().size()) return APEX_ERR_NOT_FOUND;
        *out = doc->roots()[index]; return APEX_OK;
    } catch (...) { return internal(context); }
    static apex_status node(void* context, apex_node_id id, apex_node_info* out) try {
        if (!out || out->struct_size < sizeof(*out)) return APEX_ERR_INVALID;
        auto& s = self(context); const auto* n = find(s, id);
        if (!n) return s.document ? APEX_ERR_NOT_FOUND : APEX_ERR_STATE;
        out->id = n->id; out->parent = n->parent; out->kind = string(n->block->key());
        out->child_count = n->children.size(); out->key_count = 0;
        for (const auto& child : n->block->children()) if (child.kind() == kv1::NodeKind::Value) ++out->key_count;
        return APEX_OK;
    } catch (...) { return internal(context); }
    static apex_status childAt(void* context, apex_node_id id, size_t index, apex_node_id* out) try {
        if (!out) return APEX_ERR_INVALID;
        auto& s = self(context); const auto* n = find(s, id);
        if (!n) return s.document ? APEX_ERR_NOT_FOUND : APEX_ERR_STATE;
        if (index >= n->children.size()) return APEX_ERR_NOT_FOUND;
        *out = n->children[index]; return APEX_OK;
    } catch (...) { return internal(context); }
    static apex_status keyAt(void* context, apex_node_id id, size_t index, apex_str* key, apex_str* value) try {
        if (!key || !value) return APEX_ERR_INVALID;
        auto& s = self(context); const auto* n = find(s, id);
        if (!n) return s.document ? APEX_ERR_NOT_FOUND : APEX_ERR_STATE;
        for (const auto& child : n->block->children()) {
            if (child.kind() != kv1::NodeKind::Value) continue;
            if (index-- != 0) continue;
            auto decoded = child.decodedValue();
            if (!decoded) return APEX_ERR_INTERNAL;
            s.scratch = std::move(*decoded); *key = string(child.key()); *value = string(s.scratch); return APEX_OK;
        }
        return APEX_ERR_NOT_FOUND;
    } catch (...) { return internal(context); }
    void save() {
        if (!state.parent_path().empty()) std::filesystem::create_directories(state.parent_path());
        std::ofstream file(state, std::ios::trunc);
        for (const auto& id : disabled) file << id << '\n';
        if (!file) throw std::runtime_error("cannot save plugin enabled state");
    }
    ~Impl() {
        // Destroy plugin-owned instances while their code and host APIs are live.
        for (const auto& weak : toolRetirements) if (auto retire = weak.lock()) (*retire)();
        lifetime.reset();
        for (auto it = loaded.rbegin(); it != loaded.rend(); ++it) {
            if (it->plugin->struct_size >= offsetof(apex_plugin, deactivate) + sizeof(it->plugin->deactivate) && it->plugin->deactivate) {
                Mark mark(breadcrumb, plugins[it->index].id, "deactivate");
                try { it->plugin->deactivate(it->plugin->user); } catch (...) { }
            }
            closeLibrary(it->library);
        }
    }
};

Host::Host(std::filesystem::path state, std::filesystem::path breadcrumb, std::function<void(std::string_view)> log)
    : impl_(std::make_unique<Impl>(std::move(state), std::move(breadcrumb), std::move(log))) {}
Host::~Host() = default;
void Host::discover(const std::vector<std::filesystem::path>& paths) {
    if (!impl_->loaded.empty()) throw std::logic_error("cannot rediscover loaded plugins");
    impl_->plugins.clear();
    std::set<std::string> seen;
    for (const auto& path : paths) {
        std::error_code ec;
        std::vector<std::filesystem::path> dirs;
        for (std::filesystem::directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec))
            if (it->is_directory()) dirs.push_back(it->path());
        std::sort(dirs.begin(), dirs.end());
        for (const auto& dir : dirs) {
            if (!std::filesystem::exists(dir / "plugin.kv3")) continue;
            Plugin plugin; plugin.folder = dir;
            try {
                auto parsed = kv3::parse(readFile(dir / "plugin.kv3"));
                if (!parsed) throw std::runtime_error(parsed.error().what());
                plugin.id = field(parsed->root, "id"); plugin.name = field(parsed->root, "name"); plugin.version = field(parsed->root, "version");
                if (plugin.id.empty() || plugin.id.find_first_of("\r\n/\\") != std::string::npos) throw std::runtime_error("invalid plugin id");
                if (!seen.insert(plugin.id).second) continue;
                if (const auto* abi = parsed->root.find("apex_abi")) {
                    const auto* majorValue = abi->find("major");
                    const auto* minorValue = abi->find("minor");
                    const auto* major = majorValue ? majorValue->get<std::int64_t>() : nullptr;
                    const auto* minor = minorValue ? minorValue->get<std::int64_t>() : nullptr;
                    if (!major || !minor || *major < 0 || *minor < 0)
                        throw std::runtime_error("invalid apex_abi version");
                    if (*major != APEX_PLUGIN_ABI_MAJOR)
                        throw std::runtime_error("manifest ABI major " + std::to_string(*major) + " does not match host ABI major " + std::to_string(APEX_PLUGIN_ABI_MAJOR));
                    if (*minor > APEX_PLUGIN_ABI_MINOR) throw std::runtime_error("manifest requires newer ABI minor");
                }
                const auto* entry = parsed->root.find("entry");
                const auto filename = entry ? field(*entry, platform()) : std::string{};
                if (filename.empty()) throw std::runtime_error("missing entry for " + platform());
                plugin.entry = dir / filename;
                plugin.enabled = !impl_->disabled.contains(plugin.id);
            } catch (const std::exception& e) {
                if (plugin.id.empty()) plugin.id = dir.filename().string();
                plugin.enabled = !impl_->disabled.contains(plugin.id);
                plugin.error = e.what();
            }
            impl_->plugins.push_back(std::move(plugin));
        }
    }
}
void Host::activate(bool safeMode) {
    if (safeMode) return;
    for (std::size_t i = 0; i < impl_->plugins.size(); ++i) {
        auto& item = impl_->plugins[i];
        if (!item.enabled || !item.error.empty() || item.loaded) continue;
        Library lib = openLibrary(item.entry);
        if (!lib) { item.error = "cannot load library: " + item.entry.string(); continue; }
        const auto entry = entryPoint(lib);
        if (!entry) { item.error = "missing apex_plugin_entry"; closeLibrary(lib); continue; }
        const apex_plugin* plugin = nullptr;
        try { Mark mark(impl_->breadcrumb, item.id, "entry"); plugin = entry(&impl_->host); }
        catch (...) { item.error = "exception in entry"; closeLibrary(lib); continue; }
        if (!plugin || plugin->struct_size < offsetof(apex_plugin, activate) + sizeof(plugin->activate)) {
            item.error = "invalid plugin table"; closeLibrary(lib); continue;
        }
        if (plugin->abi_major != APEX_PLUGIN_ABI_MAJOR) {
            item.error = "plugin ABI major " + std::to_string(plugin->abi_major) + " does not match host ABI major " + std::to_string(APEX_PLUGIN_ABI_MAJOR);
            closeLibrary(lib); continue;
        }
        if (plugin->abi_minor > APEX_PLUGIN_ABI_MINOR) { item.error = "plugin requires newer ABI minor"; closeLibrary(lib); continue; }
        impl_->registering = item.id;
        impl_->pending.clear();
        try {
            if (plugin->activate) { Mark mark(impl_->breadcrumb, item.id, "activate"); if (plugin->activate(plugin->user) != APEX_OK) item.error = "activation failed"; }
        } catch (...) { item.error = "exception in activate"; }
        impl_->registering.clear();
        if (!item.error.empty()) { impl_->pending.clear(); closeLibrary(lib); continue; }
        impl_->commitRegistrations();
        impl_->loaded.push_back({i, lib, plugin}); item.loaded = true;
    }
}
void Host::setPanelRegistrar(std::function<bool(std::string, std::string)> registrar) {
    impl_->panelRegistrar = std::move(registrar);
}
void Host::setUiBackend(UiBackend* backend) { impl_->uiBackend = backend; }
bool Host::describePanel(std::string_view id, const std::function<void(const apex_ui_form&)>& consume) {
    return describePanel(id, nullptr, consume);
}
bool Host::describePanel(std::string_view id, const UiContext* context, const std::function<void(const apex_ui_form&)>& consume) {
    const auto it = impl_->uiPanels.find(id);
    if (it == impl_->uiPanels.end() || !consume) return false;
    try {
        std::vector<apex_str> names;
        for (const auto& n : context ? context->names : std::vector<std::string>{}) names.push_back({n.data(), n.size()});
        apex_ui_context native{sizeof(native), {}, nullptr, 0, nullptr, 0};
        if (context) {
            native.kind = {context->kind.data(), context->kind.size()};
            native.names = names.data(); native.name_count = names.size();
            native.ids = context->ids.data(); native.id_count = context->ids.size();
        }
        const apex_ui_form* form = nullptr;
        apex_status result = APEX_ERR_INTERNAL;
        const auto& entry = it->second;
        if (!impl_->guardedUi(entry.owner, "ui.describe", [&] {
                result = entry.uiCallbacks.describe(entry.user, context ? &native : nullptr, &form);
            })) return false;
        if (result != APEX_OK || !form || form->struct_size < sizeof(apex_ui_form)) return false;
        consume(*form);
        return true;
    } catch (...) { impl_->setError("exception while describing plugin ui"); return false; }
}
std::vector<UiSection> Host::sections(std::string_view target) const {
    std::vector<UiSection> out;
    try {
        for (const auto& [id, r] : impl_->uiPanels)
            if (r.kind == Impl::Registration::Kind::Section && r.target == target) out.push_back({id, r.name});
    } catch (...) { }
    return out;
}
bool Host::hasPanel(std::string_view id) const {
    const auto it = impl_->uiPanels.find(id);
    return it != impl_->uiPanels.end() && it->second.kind == Impl::Registration::Kind::Panel;
}
void Host::panelChanged(std::string_view id, std::string_view key, const apex_ui_value& value) {
    const auto it = impl_->uiPanels.find(id);
    if (it == impl_->uiPanels.end() || !it->second.uiCallbacks.on_change) return;
    const auto& entry = it->second;
    impl_->guardedUi(entry.owner, "ui.on_change", [&] { entry.uiCallbacks.on_change(entry.user, string(key), &value); });
}
void Host::panelClicked(std::string_view id, std::string_view key) {
    const auto it = impl_->uiPanels.find(id);
    if (it == impl_->uiPanels.end() || !it->second.uiCallbacks.on_click) return;
    const auto& entry = it->second;
    impl_->guardedUi(entry.owner, "ui.on_click", [&] { entry.uiCallbacks.on_click(entry.user, string(key)); });
}
void Host::setSession(editor::EditorSession* session) {
    if (impl_->observedDocument) impl_->observedDocument->unobserve(impl_->observerToken);
    impl_->session = session;
    impl_->observedDocument = session ? session->document() : nullptr;
    if (impl_->observedDocument)
        impl_->observerToken = impl_->observedDocument->observe([impl = impl_.get(), life = std::weak_ptr(impl_->lifetime)](const document::ChangeSet& changes) { if (!life.expired()) impl->changed(changes); });
}
void Host::setDocument(const document::Document* document) {
    for (const auto& loaded : impl_->loaded) {
        if (impl_->document && loaded.plugin->struct_size >= offsetof(apex_plugin, on_document_closing) + sizeof(loaded.plugin->on_document_closing) && loaded.plugin->on_document_closing) {
            Mark mark(impl_->breadcrumb, impl_->plugins[loaded.index].id, "on_document_closing");
            try { loaded.plugin->on_document_closing(loaded.plugin->user); } catch (...) { impl_->plugins[loaded.index].error = "exception in on_document_closing"; }
        }
    }
    impl_->document = document;
    for (const auto& loaded : impl_->loaded) {
        if (document && loaded.plugin->struct_size >= offsetof(apex_plugin, on_document_opened) + sizeof(loaded.plugin->on_document_opened) && loaded.plugin->on_document_opened) {
            Mark mark(impl_->breadcrumb, impl_->plugins[loaded.index].id, "on_document_opened");
            try { loaded.plugin->on_document_opened(loaded.plugin->user); } catch (...) { impl_->plugins[loaded.index].error = "exception in on_document_opened"; }
        }
    }
}
void Host::setEnabled(std::string_view id, bool enabled) {
    const auto previous = impl_->disabled;
    if (enabled) impl_->disabled.erase(std::string(id)); else impl_->disabled.insert(std::string(id));
    try { impl_->save(); }
    catch (...) { impl_->disabled = previous; throw; }
    for (auto& p : impl_->plugins) if (p.id == id) p.enabled = enabled;
}
const std::vector<Plugin>& Host::plugins() const { return impl_->plugins; }
std::string Host::staleBreadcrumb() const { return impl_->breadcrumb.stale(); }
}
