#include "plugin_host/plugin_host.hpp"
#include "plugin_host/plugin_tool.hpp"
#include "core/entity_helpers.hpp"
#include "core/job_pool.hpp"
#include "core/kv3.hpp"
#include "core/log.hpp"
#include "core/strings.hpp"
#include "document/document.hpp"
#include "document/entity_transform.hpp"
#include "editor/editor_session.hpp"
#include "editor/map_problems.hpp"
#include "geometry/solid_block.hpp"
#include <apex/apex_plugin.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <thread>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <map>
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

// ---- apex.vfs/1: the adapter between assets::VfsProvider and a plugin's C callbacks ----
// Providers are called from host workers, so every call goes through guardedCall: it runs the plugin code on
// a helper thread, waits at most the timeout, catches exceptions, and serialises refresh/list (exclusive)
// against open (shared). A call that times out cannot be stopped; the helper thread is left to finish
// (it owns everything it touches), and the provider is out of service until it does and the game reloads.
struct ProviderCore {
    std::string id, name, owner;
    void* user = nullptr;
    apex_status (*refreshFn)(void*) = nullptr;
    apex_status (*listFn)(void*, const apex_vfs_file_sink*) = nullptr;
    apex_status (*openFn)(void*, apex_str, const apex_vfs_data_sink*) = nullptr;
    std::shared_timed_mutex gate;
    std::atomic<bool> faulted{false}, closed{false};
    std::atomic<int> inFlight{0};
    std::atomic<long long> timeoutMs{5000};
};
Error providerError(ErrorCode code, const ProviderCore& core, std::string what) {
    return Error{code, "plugin provider " + core.name + ": " + std::move(what)};
}
Error statusError(const ProviderCore& core, apex_status status, std::string_view what) {
    ErrorCode code = ErrorCode::IoError;
    if (status == APEX_ERR_NOT_FOUND) code = ErrorCode::NotFound;
    else if (status == APEX_ERR_UNSUPPORTED) code = ErrorCode::Unsupported;
    else if (status == APEX_ERR_INVALID) code = ErrorCode::InvalidArgument;
    return providerError(code, core, std::string(what) + " returned status " + std::to_string(static_cast<int>(status)));
}
template <class R, class F>
Result<R> guardedCall(const std::shared_ptr<ProviderCore>& core, bool exclusive, std::string_view what, F fn) {
    struct State {
        std::mutex mutex;
        std::condition_variable done;
        bool finished = false;
        std::optional<Result<R>> result;
    };
    core->inFlight.fetch_add(1);
    if (core->closed.load()) {
        core->inFlight.fetch_sub(1);
        return std::unexpected(providerError(ErrorCode::IoError, *core, "the host is shutting down"));
    }
    if (core->faulted.load()) {
        core->inFlight.fetch_sub(1);
        return std::unexpected(providerError(ErrorCode::IoError, *core, "out of service after a call timed out"));
    }
    const auto timeout = std::chrono::milliseconds(core->timeoutMs.load());
    auto state = std::make_shared<State>();
    try {
        std::thread([core, state, exclusive, timeout, fn = std::move(fn), label = std::string(what)]() mutable {
            Result<R> result = std::unexpected(providerError(ErrorCode::Internal, *core, label + " failed"));
            try {
                const auto lockWait = timeout / 2;
                const bool locked = exclusive ? core->gate.try_lock_for(lockWait) : core->gate.try_lock_shared_for(lockWait);
                if (!locked) {
                    result = std::unexpected(providerError(ErrorCode::IoError, *core, label + " is blocked by another call"));
                } else {
                    try { result = fn(); }
                    catch (const std::exception& e) { result = std::unexpected(providerError(ErrorCode::Internal, *core, label + " threw: " + e.what())); }
                    catch (...) { result = std::unexpected(providerError(ErrorCode::Internal, *core, label + " threw")); }
                    if (exclusive) core->gate.unlock(); else core->gate.unlock_shared();
                }
            } catch (...) {}
            {
                std::lock_guard lock(state->mutex);
                state->result = std::move(result);
                state->finished = true;
            }
            state->done.notify_all();
            core->inFlight.fetch_sub(1);
        }).detach();
    } catch (...) {
        core->inFlight.fetch_sub(1);
        return std::unexpected(providerError(ErrorCode::Internal, *core, "cannot start a thread for " + std::string(what)));
    }
    std::unique_lock lock(state->mutex);
    if (!state->done.wait_for(lock, timeout, [&] { return state->finished; })) {
        core->faulted.store(true);
        log::warn("plugin {}: provider {}: {} did not return within {} ms; the provider is out of service",
                  core->owner, core->name, what, timeout.count());
        return std::unexpected(providerError(ErrorCode::IoError, *core, std::string(what) + " timed out"));
    }
    return std::move(*state->result);
}
struct FileSinkState { std::vector<std::string> paths; bool truncated = false; };
struct DataSinkState { assets::Blob bytes; std::size_t limit = 0; };
constexpr std::size_t kMaxProviderFiles = 200000, kMaxProviderPath = 1024;
class PluginVfsProvider final : public assets::VfsProvider {
public:
    explicit PluginVfsProvider(std::shared_ptr<ProviderCore> core) : core_(std::move(core)) {}
    std::string name() const override { return core_->name; }
    Result<void> refresh() override {
        if (core_->faulted.load()) {
            if (core_->inFlight.load() != 0)
                return std::unexpected(providerError(ErrorCode::IoError, *core_, "still stuck in an earlier call"));
            core_->faulted.store(false);  // the stuck call returned: give the provider another chance
        }
        if (!core_->refreshFn) return {};
        auto core = core_;
        return guardedCall<void>(core_, true, "refresh", [core]() -> Result<void> {
            const auto status = core->refreshFn(core->user);
            if (status != APEX_OK) return std::unexpected(statusError(*core, status, "refresh"));
            return {};
        });
    }
    Result<std::vector<std::string>> files() override {
        auto core = core_;
        return guardedCall<std::vector<std::string>>(core_, true, "list", [core]() -> Result<std::vector<std::string>> {
            FileSinkState state;
            const apex_vfs_file_sink sink{sizeof(sink), &state, [](void* p, apex_str path) -> apex_status {
                try {
                    auto& out = *static_cast<FileSinkState*>(p);
                    const auto text = view(path);
                    if (text.empty() || text.size() > kMaxProviderPath) return APEX_ERR_INVALID;
                    if (out.paths.size() >= kMaxProviderFiles) { out.truncated = true; return APEX_ERR_STATE; }
                    out.paths.emplace_back(text);
                    return APEX_OK;
                } catch (...) { return APEX_ERR_INTERNAL; }
            }};
            const auto status = core->listFn(core->user, &sink);
            if (state.truncated) log::warn("plugin {}: provider {} listed more than {} files; the rest are ignored",
                                           core->owner, core->name, kMaxProviderFiles);
            if (status != APEX_OK) return std::unexpected(statusError(*core, status, "list"));
            return std::move(state.paths);
        });
    }
    Result<assets::Blob> open(std::string_view path, std::size_t maxBytes) override {
        auto core = core_;
        return guardedCall<assets::Blob>(core_, false, "open", [core, name = std::string(path), maxBytes]() -> Result<assets::Blob> {
            DataSinkState state;
            state.limit = maxBytes;
            const apex_vfs_data_sink sink{sizeof(sink), &state, maxBytes,
                [](void* p, const void* data, size_t size) -> apex_status {
                    try {
                        auto& out = *static_cast<DataSinkState*>(p);
                        if (size && !data) return APEX_ERR_INVALID;
                        const auto room = out.limit - std::min(out.limit, out.bytes.size());
                        const auto take = std::min(room, size);
                        const auto* bytes = static_cast<const std::byte*>(data);
                        out.bytes.insert(out.bytes.end(), bytes, bytes + take);
                        return APEX_OK;
                    } catch (...) { return APEX_ERR_INTERNAL; }
                }};
            const auto status = core->openFn(core->user, string(name), &sink);
            if (status != APEX_OK) return std::unexpected(statusError(*core, status, "open"));
            return std::move(state.bytes);
        });
    }
private:
    std::shared_ptr<ProviderCore> core_;
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
    struct ToolRetirement { std::string owner, tool; std::weak_ptr<std::function<void()>> retire; std::weak_ptr<PluginTool::Revive> revive; };
    std::vector<ToolRetirement> toolRetirements;
    // Script plugins (0.9.7): one entry per enabled script plugin, kept even when its load failed so a later
    // save can reload it.
    struct ScriptEntry { Impl* host; std::size_t index; std::unique_ptr<ScriptEngine> engine; };
    std::vector<std::unique_ptr<ScriptEntry>> scripts;
    ScriptEngineFactory scriptFactory;
    bool reloading = false;
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
    apex_overlay_v1 overlayApi{};
    apex_helpers_v1 helpersApi{};
    apex_problems_v1 problemsApi{};
    apex_io_v1 ioApi{};
    apex_vfs_v1 vfsApi{};
    apex_assets_v1 assetsApi{};
    apex_jobs_v1 jobsApi{};
    apex_watch_v1 watchApi{};
    editor::ProblemRegistry* problems = &editor::mutableProblemRegistry();
    std::vector<OverlayData> overlays;                  // declaration order
    std::map<std::string, std::string, std::less<>> overlayOwner;
    std::uint64_t overlayRevision = 0;
    struct HelperRegistration { std::string owner; void* user; apex_helper_draw draw; };
    std::map<std::string, HelperRegistration, std::less<>> helperRegistrations;  // by lower-case FGD name
    HelperDrawing helperCache;
    const document::Document* helperDocument = nullptr;
    std::uint64_t helperState = 0, helperClasses = 0;
    bool helperValid = false;
    UiBackend* uiBackend = nullptr;
    // apex.io/1
    struct ImporterEntry { ImporterInfo info; void* user; apex_import_callback import; };
    std::vector<ImporterEntry> importerEntries;
    std::vector<ImporterInfo> importerInfos;
    // apex.vfs/1 and apex.assets/1
    struct VfsRegistration { std::string id, owner; std::shared_ptr<ProviderCore> core; assets::VfsPosition position; };
    mutable std::mutex vfsMutex;                       // registrations are read from any thread
    std::vector<VfsRegistration> vfsRegistrations;
    std::chrono::milliseconds vfsTimeout{5000};
    std::function<void(std::string_view)> vfsRefreshHandler;
    AssetsBackend* assetsBackend = nullptr;
    MaterialRecord materialScratch;
    // apex.jobs/1: thread-safe submission; completions are delivered by drainJobs on the editor thread.
    std::thread::id mainThread = std::this_thread::get_id();
    mutable std::mutex jobMutex;
    std::unique_ptr<JobPool> jobPool;
    std::map<std::uint64_t, JobHandle> jobHandles;
    std::uint64_t lastJob = 0;
    bool jobsClosed = false;
    static inline thread_local const std::string* jobOwner = nullptr;
    // apex.watch/1: polled on the editor thread.
    struct Watch {
        std::uint64_t id; std::string owner; std::filesystem::path path;
        void* user; apex_watch_callback fn;
        std::uint64_t signature = 0; bool pending = false;
        std::chrono::steady_clock::time_point since{};
    };
    std::vector<Watch> watches;
    std::vector<std::filesystem::path> watchRoots;
    std::chrono::milliseconds debounce{200};
    std::uint64_t lastWatch = 0;
    std::string registering;
    std::string callbackOwner;
    std::function<bool(std::string, std::string)> panelRegistrar;
    struct Registration {
        enum class Kind { Action, Tool, Panel, Section, Dialog, Problem } kind;
        std::string id, name, description, owner, target;
        void* user;
        apex_callback first, second;
        bool extended = false;
        apex_tool_callbacks callbacks{};
        apex_ui_callbacks uiCallbacks{};
        apex_problem_check problemCheck = nullptr;
        std::map<std::string, std::string, std::less<>> suggestions, suggestionContexts;
        bool dead = false;   // a script reload neutralised it; its registry entry stays but does nothing
    };
    std::vector<Registration> pending;
    // What the registries' entries call through, by id. Reloading a script rebinds a cell instead of
    // adding a second registry entry (the registries have no remove and freeze after startup).
    std::map<std::string, std::shared_ptr<Registration>, std::less<>> cells;
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
        overlayApi = {sizeof(overlayApi), declareOverlay, setOverlay, clearOverlay};
        helpersApi = {sizeof(helpersApi), registerHelper};
        problemsApi = {sizeof(problemsApi), registerProblemCheck};
        ioApi = {sizeof(ioApi), registerImporter};
        vfsApi = {sizeof(vfsApi), registerVfsProvider, requestVfsRefresh};
        assetsApi = {sizeof(assetsApi), listMaterials, findMaterial, writeVmt};
        jobsApi = {sizeof(jobsApi), submitJob, cancelJob};
        watchApi = {sizeof(watchApi), watchPath, unwatchPath};
    }
    void setError(const char* message) noexcept {
        if (std::this_thread::get_id() != mainThread) return;  // lastError is editor-thread state
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
        if (view(name) == "apex.overlay/1") return &self(context).overlayApi;
        if (view(name) == "apex.helpers/1") return &self(context).helpersApi;
        if (view(name) == "apex.problems/1") return &self(context).problemsApi;
        if (view(name) == "apex.io/1") return &self(context).ioApi;
        if (view(name) == "apex.vfs/1") return &self(context).vfsApi;
        if (view(name) == "apex.assets/1") return &self(context).assetsApi;
        if (view(name) == "apex.jobs/1") return &self(context).jobsApi;
        if (view(name) == "apex.watch/1") return &self(context).watchApi;
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
    static const apex_edit_ops_v1& editOps() {
        static const apex_edit_ops_v1 ops{sizeof(ops), setKey, destroyNode, createSolid,
                                           reparentNode, setSideKey, removeDisplacement, rebuildSolid};
        return ops;
    }
    static apex_status applyEdit(void* c, apex_str label, apex_str coalesce, void* user, apex_edit_callback callback) try {
        auto& s = self(c);
        if (!s.session || !s.session->document() || !callback) return APEX_ERR_STATE;
        const auto& ops = editOps();
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
        for (const auto& entry : scripts) {
            if (!entry->engine) continue;
            const auto& owner = plugins[entry->index].id;
            Mark mark(breadcrumb, owner, "script.on_changed");
            const auto previous = std::exchange(callbackOwner, owner);
            try { entry->engine->documentChanged(notice); }
            catch (...) { setError("exception in script on_changed"); }
            callbackOwner = previous;
        }
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
        if (s.registering.empty() || (!s.reloading && (editor::actionRegistry().frozen() || editor::toolRegistry().frozen())))
            return APEX_ERR_STATE;
        const auto idText = view(id);
        const bool rebinding = s.reloading && s.cells.contains(idText) && s.cells.find(idText)->second->owner == s.registering;
        if (!inNamespace(s.registering, idText) || view(name).empty()) {
            s.lastError = "registration outside plugin namespace or missing name";
            return APEX_ERR_INVALID;
        }
        if (std::ranges::any_of(s.pending, [&](const auto& p) { return p.id == idText; }) ||
            (!rebinding && s.uiPanels.contains(idText)) ||
            (!rebinding && (kind == Registration::Kind::Action || kind == Registration::Kind::Tool) && editor::actionRegistry().find(idText)) ||
            (!rebinding && kind == Registration::Kind::Tool && std::ranges::any_of(editor::toolRegistry().entries(),
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
    // ---- apex.overlay/1, apex.helpers/1, apex.problems/1 (0.9.5) ----
    std::string actingOwner() const { return callbackOwner.empty() ? registering : callbackOwner; }
    static bool finite(const apex_vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
    static DVec3 vec(const apex_vec3& v) { return {v.x, v.y, v.z}; }
    static bool rgba(const float c[4], Vec4& out) {
        for (int i = 0; i < 4; ++i) if (!std::isfinite(c[i])) return false;
        out = {c[0], c[1], c[2], c[3]};
        return true;
    }
    static bool segmentOf(const apex_tool_segment& in, OverlaySegment& out) {
        Vec4 colour;
        if (!finite(in.a) || !finite(in.b) || !rgba(in.rgba, colour) || !std::isfinite(in.width)) return false;
        out = {vec(in.a), vec(in.b), colour, std::clamp(in.width, 0.5f, 64.0f)};
        return true;
    }
    static bool labelOf(const apex_overlay_label& in, OverlayLabel& out) {
        Vec4 colour;
        if (!finite(in.position) || !rgba(in.rgba, colour) || (!in.text.data && in.text.size)) return false;
        out = {vec(in.position), std::string(view(in.text).substr(0, 256)), colour};
        return true;
    }
    static constexpr std::size_t kMaxOverlayItems = 65536, kMaxOverlayTints = 262144, kMaxOverlayGhosts = 256;
    static apex_status declareOverlay(void* c, apex_str id, apex_str name) try {
        auto& s = self(c);
        if (s.registering.empty()) return APEX_ERR_STATE;
        if (!inNamespace(s.registering, view(id)) || view(name).empty()) {
            s.setError("overlay id outside plugin namespace or missing name");
            return APEX_ERR_INVALID;
        }
        if (s.overlayOwner.contains(view(id))) { s.setError("duplicate overlay"); return APEX_ERR_INVALID; }
        s.overlayOwner.emplace(std::string(view(id)), s.registering);
        OverlayData entry;
        entry.id = std::string(view(id));
        entry.name = std::string(view(name));
        s.overlays.push_back(std::move(entry));
        ++s.overlayRevision;
        return APEX_OK;
    } catch (...) { return internal(c); }
    OverlayData* ownedOverlay(std::string_view id, apex_status& status) {
        const auto owner = actingOwner();
        const auto found = overlayOwner.find(id);
        if (owner.empty()) { status = APEX_ERR_STATE; return nullptr; }
        if (found == overlayOwner.end()) { status = APEX_ERR_NOT_FOUND; return nullptr; }
        if (found->second != owner) { setError("overlay belongs to another plugin"); status = APEX_ERR_INVALID; return nullptr; }
        const auto it = std::ranges::find(overlays, id, &OverlayData::id);
        if (it == overlays.end()) { status = APEX_ERR_NOT_FOUND; return nullptr; }
        status = APEX_OK;
        return &*it;
    }
    static apex_status setOverlay(void* c, apex_str id, const apex_overlay_list* list) try {
        auto& s = self(c);
        apex_status status = APEX_OK;
        auto* entry = s.ownedOverlay(view(id), status);
        if (!entry) return status;
        if (!list || list->struct_size < offsetof(apex_overlay_list, segment_count) + sizeof(list->segment_count))
            return APEX_ERR_INVALID;
#define APEX_HAS(field) (list->struct_size >= offsetof(apex_overlay_list, field) + sizeof(list->field))
#define APEX_BAD(count, ptr, limit) ((count) > (limit) || ((count) && !(ptr)))
        OverlayData next;
        next.id = entry->id;
        next.name = entry->name;
        if (APEX_HAS(segment_count)) {
            if (APEX_BAD(list->segment_count, list->segments, kMaxOverlayItems)) return APEX_ERR_INVALID;
            for (std::size_t i = 0; i < list->segment_count; ++i) {
                OverlaySegment out;
                if (!segmentOf(list->segments[i], out)) { s.setError("invalid overlay segment"); return APEX_ERR_INVALID; }
                next.segments.push_back(out);
            }
        }
        if (APEX_HAS(point_count)) {
            if (APEX_BAD(list->point_count, list->points, kMaxOverlayItems)) return APEX_ERR_INVALID;
            for (std::size_t i = 0; i < list->point_count; ++i) {
                const auto& in = list->points[i];
                Vec4 colour;
                if (!finite(in.position) || !rgba(in.rgba, colour) || !std::isfinite(in.size)) {
                    s.setError("invalid overlay point");
                    return APEX_ERR_INVALID;
                }
                next.points.push_back({vec(in.position), colour, std::clamp(in.size, 1.0f, 64.0f)});
            }
        }
        if (APEX_HAS(label_count)) {
            if (APEX_BAD(list->label_count, list->labels, kMaxOverlayItems)) return APEX_ERR_INVALID;
            for (std::size_t i = 0; i < list->label_count; ++i) {
                OverlayLabel out;
                if (!labelOf(list->labels[i], out)) { s.setError("invalid overlay label"); return APEX_ERR_INVALID; }
                next.labels.push_back(std::move(out));
            }
        }
        if (APEX_HAS(ghost_count)) {
            if (APEX_BAD(list->ghost_count, list->ghosts, kMaxOverlayGhosts)) return APEX_ERR_INVALID;
            for (std::size_t i = 0; i < list->ghost_count; ++i) {
                const auto& ghost = list->ghosts[i];
                Vec4 colour;
                if (!ghost.planes || ghost.plane_count < 4 || ghost.plane_count > 256 || !rgba(ghost.rgba, colour)) {
                    s.setError("invalid overlay ghost");
                    return APEX_ERR_INVALID;
                }
                std::vector<geometry::PlaneSide> planes;
                for (std::size_t j = 0; j < ghost.plane_count; ++j) {
                    if (!finite(ghost.planes[j].normal) || !std::isfinite(ghost.planes[j].distance)) {
                        s.setError("invalid overlay ghost plane");
                        return APEX_ERR_INVALID;
                    }
                    geometry::PlaneSide side;
                    side.plane = {vec(ghost.planes[j].normal), ghost.planes[j].distance};
                    planes.push_back(std::move(side));
                }
                auto poly = geometry::ConvexPolyhedron::fromPlanes(planes);
                if (!poly) { s.setError("overlay ghost planes do not close into a solid"); return APEX_ERR_INVALID; }
                const Vec4 edge{colour.r, colour.g, colour.b, std::max(colour.a, 0.6f)};
                for (const auto& face : poly->faces) {
                    for (std::size_t k = 0; k < face.vertices.size(); ++k) {
                        const auto from = face.vertices[k], to = face.vertices[(k + 1) % face.vertices.size()];
                        if (from < to) // each edge is shared by two faces: draw it once
                            next.segments.push_back({poly->vertices[from], poly->vertices[to], edge, 1.5f});
                        if (k >= 2) next.triangles.push_back({poly->vertices[face.vertices[0]],
                                                              poly->vertices[face.vertices[k - 1]],
                                                              poly->vertices[face.vertices[k]], colour});
                    }
                }
            }
        }
        if (APEX_HAS(tint_count)) {
            if (APEX_BAD(list->tint_count, list->tints, kMaxOverlayTints)) return APEX_ERR_INVALID;
            for (std::size_t i = 0; i < list->tint_count; ++i) {
                Vec4 colour;
                if (!rgba(list->tints[i].rgba, colour)) { s.setError("invalid overlay tint"); return APEX_ERR_INVALID; }
                next.tints[list->tints[i].side] = colour;
            }
        }
#undef APEX_HAS
#undef APEX_BAD
        *entry = std::move(next);
        ++s.overlayRevision;
        return APEX_OK;
    } catch (...) { return internal(c); }
    static apex_status clearOverlay(void* c, apex_str id) try {
        auto& s = self(c);
        apex_status status = APEX_OK;
        auto* entry = s.ownedOverlay(view(id), status);
        if (!entry) return status;
        OverlayData empty;
        empty.id = entry->id;
        empty.name = entry->name;
        *entry = std::move(empty);
        ++s.overlayRevision;
        return APEX_OK;
    } catch (...) { return internal(c); }
    void dropOverlayData() {
        for (auto& entry : overlays) {
            entry.segments.clear(); entry.points.clear(); entry.triangles.clear(); entry.labels.clear(); entry.tints.clear();
        }
        ++overlayRevision;
    }
    static apex_status registerHelper(void* c, apex_str name, void* user, apex_helper_draw draw) try {
        auto& s = self(c);
        if (s.registering.empty()) return APEX_ERR_STATE;
        const auto lower = toLowerAscii(view(name));
        if (!draw || lower.empty() ||
            !std::ranges::all_of(lower, [](unsigned char ch) { return std::isalnum(ch) || ch == '_'; })) return APEX_ERR_INVALID;
        if (!std::holds_alternative<assets::UnknownHelper>(assets::parseHelper({lower, {}}))) {
            s.setError("helper is built into the host");
            return APEX_ERR_INVALID;
        }
        if (s.helperRegistrations.contains(lower)) { s.setError("helper already registered"); return APEX_ERR_INVALID; }
        s.helperRegistrations.emplace(lower, HelperRegistration{s.registering, user, draw});
        s.helperValid = false;
        return APEX_OK;
    } catch (...) { return internal(c); }
    static apex_status registerProblemCheck(void* c, apex_str id, apex_str name, void* user, apex_problem_check check) try {
        auto& s = self(c);
        if (!check) return APEX_ERR_INVALID;
        const bool rebinding = s.reloading && s.cells.contains(view(id));
        if (s.problems->frozen() && !s.reloading) return APEX_ERR_STATE;
        if (!rebinding && std::ranges::find(s.problems->names(), std::string(view(id))) != s.problems->names().end()) {
            s.setError("duplicate registration");
            return APEX_ERR_INVALID;
        }
        const auto result = registerItem(c, Registration::Kind::Problem, id, name, {}, user, nullptr, nullptr);
        if (result != APEX_OK) return result;
        s.pending.back().problemCheck = check;
        return APEX_OK;
    } catch (...) { return internal(c); }

    // ---- apex.io/1, apex.jobs/1, apex.watch/1 (0.9.5b) ----
    static apex_status registerImporter(void* c, apex_str id, apex_str name, apex_str extensions, void* user,
                                        apex_import_callback fn) try {
        auto& s = self(c);
        if (s.registering.empty()) return APEX_ERR_STATE;
        if (!fn || !inNamespace(s.registering, view(id)) || view(name).empty()) {
            s.setError("importer id outside plugin namespace, missing name or callback");
            return APEX_ERR_INVALID;
        }
        if (std::ranges::any_of(s.importerEntries, [&](const auto& e) { return e.info.id == view(id); })) {
            s.setError("duplicate importer");
            return APEX_ERR_INVALID;
        }
        ImporterInfo info;
        info.id = std::string(view(id));
        info.name = std::string(view(name));
        info.owner = s.registering;
        const auto list = view(extensions);
        for (std::size_t begin = 0; begin <= list.size();) {
            auto end = list.find(',', begin);
            if (end == std::string_view::npos) end = list.size();
            auto ext = trim(list.substr(begin, end - begin));
            if (ext.starts_with('.')) ext.remove_prefix(1);
            if (!ext.empty()) {
                if (!std::ranges::all_of(ext, [](unsigned char ch) { return std::isalnum(ch) || ch == '_' || ch == '-'; })) {
                    s.setError("invalid importer extension");
                    return APEX_ERR_INVALID;
                }
                info.extensions.push_back(toLowerAscii(ext));
            }
            begin = end + 1;
        }
        if (info.extensions.empty()) { s.setError("importer needs at least one extension"); return APEX_ERR_INVALID; }
        s.importerEntries.push_back({info, user, fn});
        s.importerInfos.push_back(std::move(info));
        return APEX_OK;
    } catch (...) { return internal(c); }
    // ---- apex.vfs/1, apex.assets/1 (0.9.6) ----
    static apex_status registerVfsProvider(void* c, apex_str id, apex_str name, const apex_vfs_provider* provider,
                                           const apex_vfs_position* position) try {
        auto& s = self(c);
        if (s.registering.empty()) return APEX_ERR_STATE;
        if (!provider || provider->struct_size < offsetof(apex_vfs_provider, open) + sizeof(provider->open) ||
            !provider->list || !provider->open || !inNamespace(s.registering, view(id)) || view(name).empty()) {
            s.setError("vfs provider id outside plugin namespace, missing name, list or open");
            return APEX_ERR_INVALID;
        }
        assets::VfsPosition place;
        if (position) {
            if (position->struct_size < offsetof(apex_vfs_position, anchor) + sizeof(position->anchor) || position->kind > 3 ||
                (position->kind >= 2 && view(position->anchor).empty())) {
                s.setError("invalid vfs position");
                return APEX_ERR_INVALID;
            }
            place.kind = static_cast<assets::VfsPosition::Kind>(position->kind);
            place.anchor = std::string(view(position->anchor));
        }
        std::lock_guard lock(s.vfsMutex);
        if (std::ranges::any_of(s.vfsRegistrations, [&](const auto& r) { return r.id == view(id); })) {
            s.setError("duplicate vfs provider");
            return APEX_ERR_INVALID;
        }
        if (s.vfsRegistrations.size() >= 16) { s.setError("too many vfs providers"); return APEX_ERR_STATE; }
        auto core = std::make_shared<ProviderCore>();
        core->id = std::string(view(id));
        core->name = std::string(view(name));
        core->owner = s.registering;
        core->user = provider->user;
        core->refreshFn = provider->refresh;
        core->listFn = provider->list;
        core->openFn = provider->open;
        core->timeoutMs.store(s.vfsTimeout.count());
        s.vfsRegistrations.push_back({core->id, core->owner, std::move(core), std::move(place)});
        return APEX_OK;
    } catch (...) { return internal(c); }
    static apex_status requestVfsRefresh(void* c, apex_str id) try {
        auto& s = self(c);
        const auto owner = s.actingOwner();
        if (owner.empty() || std::this_thread::get_id() != s.mainThread) return APEX_ERR_STATE;
        {
            std::lock_guard lock(s.vfsMutex);
            const auto found = std::ranges::find_if(s.vfsRegistrations, [&](const auto& r) { return r.id == view(id); });
            if (found == s.vfsRegistrations.end()) return APEX_ERR_NOT_FOUND;
            if (found->owner != owner) { s.setError("vfs provider belongs to another plugin"); return APEX_ERR_INVALID; }
        }
        if (!s.vfsRefreshHandler) { s.setError("no game is loaded"); return APEX_ERR_STATE; }
        s.vfsRefreshHandler(view(id));
        return APEX_OK;
    } catch (...) { return internal(c); }
    static apex_material_info materialInfo(const MaterialRecord& m) {
        return {sizeof(apex_material_info), string(m.name), string(m.shader), string(m.baseTexture), string(m.surfaceProp), string(m.source)};
    }
    static apex_status listMaterials(void* c, void* user, apex_material_visit visit) try {
        auto& s = self(c);
        if (std::this_thread::get_id() != s.mainThread || !s.assetsBackend) return APEX_ERR_STATE;
        if (!visit) return APEX_ERR_INVALID;
        s.assetsBackend->materials([&](const MaterialRecord& m) {
            const auto info = materialInfo(m);
            return visit(user, &info) == 0;
        });
        return APEX_OK;
    } catch (...) { return internal(c); }
    static apex_status findMaterial(void* c, apex_str name, apex_material_info* out) try {
        auto& s = self(c);
        if (std::this_thread::get_id() != s.mainThread || !s.assetsBackend) return APEX_ERR_STATE;
        if (!out || out->struct_size < sizeof(*out) || view(name).empty()) return APEX_ERR_INVALID;
        auto found = s.assetsBackend->material(view(name));
        if (!found) return APEX_ERR_NOT_FOUND;
        s.materialScratch = std::move(*found);
        *out = materialInfo(s.materialScratch);
        return APEX_OK;
    } catch (...) { return internal(c); }
    static apex_status writeVmt(void* c, apex_str material, const apex_vmt_edit* edits, size_t count) try {
        auto& s = self(c);
        if (std::this_thread::get_id() != s.mainThread || !s.assetsBackend) return APEX_ERR_STATE;
        if (view(material).empty() || !edits || count == 0 || count > 256) { s.setError("write_vmt needs a material and 1 to 256 edits"); return APEX_ERR_INVALID; }
        std::vector<VmtEditRequest> requests;
        for (size_t i = 0; i < count; ++i) {
            const auto& edit = edits[i];
            const auto key = view(edit.key);
            const auto value = view(edit.value);
            if (edit.struct_size < sizeof(apex_vmt_edit) || key.empty() || key.size() > 256 || value.size() > 4096 ||
                std::ranges::any_of(key, [](unsigned char ch) { return ch <= ' ' || ch == '"' || ch == '{' || ch == '}'; }) ||
                std::ranges::any_of(value, [](unsigned char ch) { return ch < ' ' && ch != '\t'; })) {
                s.setError("invalid vmt edit");
                return APEX_ERR_INVALID;
            }
            requests.push_back({std::string(key), std::string(value), edit.remove != 0});
        }
        const auto result = s.assetsBackend->writeVmt(view(material), requests);
        if (result.ok) return APEX_OK;
        s.setError(result.message.c_str());
        return result.unsupported ? APEX_ERR_UNSUPPORTED : result.notFound ? APEX_ERR_NOT_FOUND : APEX_ERR_STATE;
    } catch (...) { return internal(c); }
    struct ImportSink { std::vector<std::string> messages; };
    static apex_status reportImport(void* sink, apex_str message) noexcept {
        try {
            auto& out = static_cast<ImportSink*>(sink)->messages;
            if (out.size() >= 100) return APEX_ERR_STATE;
            out.emplace_back(view(message).substr(0, 1024));
            return APEX_OK;
        } catch (...) { return APEX_ERR_INTERNAL; }
    }
    static void finishJob(Impl& s, const std::weak_ptr<bool>& life, std::uint64_t id, void* user, apex_job_done done,
                          const std::string& owner, apex_status status) {
        if (life.expired()) return;
        { std::lock_guard lock(s.jobMutex); s.jobHandles.erase(id); }
        Mark mark(s.breadcrumb, owner, "job.done");
        const auto previous = std::exchange(s.callbackOwner, owner);
        try { done(user, status); } catch (...) { s.lastError = "exception in job completion"; }
        s.callbackOwner = previous;
    }
    static apex_status submitJob(void* c, void* user, apex_job_work work, apex_job_done done, uint64_t* out) try {
        auto& s = self(c);
        if (!work || !done || !out) return APEX_ERR_INVALID;
        // The owner names the plugin in the crash breadcrumb. A worker thread inherits its job's owner.
        std::string owner = std::this_thread::get_id() == s.mainThread ? s.actingOwner() : (jobOwner ? *jobOwner : std::string{});
        if (owner.empty()) owner = "jobs";
        std::lock_guard lock(s.jobMutex);
        if (s.jobsClosed) return APEX_ERR_STATE;
        if (!s.jobPool) s.jobPool = std::make_unique<JobPool>(std::clamp(std::thread::hardware_concurrency() / 2u, 1u, 4u));
        const auto id = ++s.lastJob;
        std::weak_ptr<bool> life = s.lifetime;
        auto handle = s.jobPool->submit(Priority::Normal,
            [life, user, work, owner](CancelToken token) -> apex_status {
                if (life.expired() || token.cancelled()) return APEX_ERR_STATE;
                const auto* previous = std::exchange(jobOwner, &owner);
                apex_status status = APEX_ERR_INTERNAL;
                try {
                    const apex_job_context context{sizeof(context), &token,
                        [](void* job) -> int { return static_cast<CancelToken*>(job)->cancelled() ? 1 : 0; }};
                    status = work(user, &context);
                } catch (...) { status = APEX_ERR_INTERNAL; }
                jobOwner = previous;
                return status;
            },
            [&s, life, id, user, done, owner](apex_status status) { finishJob(s, life, id, user, done, owner, status); });
        s.jobHandles.emplace(id, std::move(handle));
        *out = id;
        return APEX_OK;
    } catch (...) { return internal(c); }
    static apex_status cancelJob(void* c, uint64_t id) try {
        auto& s = self(c);
        std::lock_guard lock(s.jobMutex);
        const auto found = s.jobHandles.find(id);
        if (found == s.jobHandles.end()) return APEX_ERR_NOT_FOUND;
        found->second.cancel();
        s.jobHandles.erase(found);
        return APEX_OK;
    } catch (...) { return internal(c); }
    static bool within(const std::filesystem::path& root, const std::filesystem::path& path) {
        const auto relative = path.lexically_relative(root);
        return !relative.empty() && *relative.begin() != "..";
    }
    static std::filesystem::path canonical(const std::filesystem::path& path) {
        std::error_code ec;
        auto result = std::filesystem::weakly_canonical(path, ec);
        return ec ? path.lexically_normal() : result;
    }
    // A cheap fingerprint of a file or a directory tree: it changes when a size, a modification time, a
    // name or existence changes. Order-independent, so directory iteration order does not matter.
    static std::uint64_t signatureOf(const std::filesystem::path& path) {
        constexpr std::uint64_t kMissing = 0x9e3779b97f4a7c15ull;
        const auto mix = [](std::uint64_t h, std::uint64_t v) { return (h ^ v) * 1099511628211ull + 0x9e3779b97f4a7c15ull; };
        const auto entry = [&](const std::filesystem::path& p, const std::filesystem::path& rel, bool directory) {
            std::error_code ec;
            std::uint64_t h = std::hash<std::string>{}(rel.generic_string());
            if (!directory) {
                h = mix(h, std::filesystem::file_size(p, ec));
                ec.clear();
            }
            h = mix(h, static_cast<std::uint64_t>(std::filesystem::last_write_time(p, ec).time_since_epoch().count()));
            return h;
        };
        std::error_code ec;
        const auto status = std::filesystem::status(path, ec);
        if (ec || !std::filesystem::exists(status)) return kMissing;
        if (!std::filesystem::is_directory(status)) return entry(path, {}, false);
        std::uint64_t total = 1, seen = 0;
        for (std::filesystem::recursive_directory_iterator it(path, std::filesystem::directory_options::skip_permission_denied, ec), end;
             !ec && it != end && seen < 10000; it.increment(ec), ++seen) {
            std::error_code inner;
            total += entry(it->path(), it->path().lexically_relative(path), it->is_directory(inner));
        }
        return total;
    }
    static apex_status watchPath(void* c, apex_str path, void* user, apex_watch_callback fn, uint64_t* out) try {
        auto& s = self(c);
        const auto owner = s.actingOwner();
        if (owner.empty() || std::this_thread::get_id() != s.mainThread) return APEX_ERR_STATE;
        if (!fn || !out || view(path).empty()) return APEX_ERR_INVALID;
        const std::filesystem::path requested{std::string(view(path))};
        if (!requested.is_absolute()) { s.setError("watch path must be absolute"); return APEX_ERR_INVALID; }
        const auto target = canonical(requested);
        bool allowed = std::ranges::any_of(s.watchRoots, [&](const auto& root) { return within(root, target); });
        for (const auto& plugin : s.plugins)
            if (plugin.id == owner && within(canonical(plugin.folder), target)) allowed = true;
        if (!allowed) { s.setError("watch path is outside the plugin folder and the game directory"); return APEX_ERR_INVALID; }
        if (s.watches.size() >= 256) { s.setError("too many watches"); return APEX_ERR_STATE; }
        Watch watch{++s.lastWatch, owner, target, user, fn};
        watch.signature = signatureOf(target);
        *out = watch.id;
        s.watches.push_back(std::move(watch));
        return APEX_OK;
    } catch (...) { return internal(c); }
    static apex_status unwatchPath(void* c, uint64_t id) try {
        auto& s = self(c);
        const auto found = std::ranges::find(s.watches, id, &Watch::id);
        if (found == s.watches.end()) return APEX_ERR_NOT_FOUND;
        if (found->owner != s.actingOwner()) { s.setError("watch belongs to another plugin"); return APEX_ERR_INVALID; }
        s.watches.erase(found);
        return APEX_OK;
    } catch (...) { return internal(c); }
    // Drops what a plugin registered directly when its activation failed.
    void forgetOwner(const std::string& owner) {
        std::erase_if(overlays, [&](const OverlayData& o) { const auto f = overlayOwner.find(o.id); return f != overlayOwner.end() && f->second == owner; });
        std::erase_if(overlayOwner, [&](const auto& item) { return item.second == owner; });
        std::erase_if(helperRegistrations, [&](const auto& item) { return item.second.owner == owner; });
        std::erase_if(importerEntries, [&](const auto& item) { return item.info.owner == owner; });
        importerInfos.clear();
        for (const auto& item : importerEntries) importerInfos.push_back(item.info);
        std::erase_if(watches, [&](const Watch& w) { return w.owner == owner; });
        {
            std::lock_guard lock(vfsMutex);
            for (auto& r : vfsRegistrations) if (r.owner == owner) r.core->closed.store(true);
            std::erase_if(vfsRegistrations, [&](const VfsRegistration& r) { return r.owner == owner; });
        }
        helperValid = false;
        ++overlayRevision;
    }
    struct ProblemSink {
        Impl& host;
        const document::Document& doc;
        std::vector<editor::Problem>& out;
        std::string owner;
        std::size_t count = 0;
    };
    static apex_status reportProblem(void* sinkPointer, const apex_problem* problem) try {
        auto& sink = *static_cast<ProblemSink*>(sinkPointer);
        if (!problem || problem->struct_size < offsetof(apex_problem, object_count) + sizeof(problem->object_count) ||
            problem->severity > 2 || view(problem->text).empty() || (problem->object_count && !problem->objects) ||
            problem->object_count > 10000) return APEX_ERR_INVALID;
        if (sink.count >= 1000) return APEX_ERR_STATE;
        editor::Problem out;
        out.severity = static_cast<editor::ProblemSeverity>(problem->severity);
        out.kind = editor::ProblemKind::Plugin;
        out.text = std::string(view(problem->text).substr(0, 1024));
        out.source = sink.owner;
        for (std::size_t i = 0; i < problem->object_count; ++i) {
            if (!sink.doc.handle(problem->objects[i])) return APEX_ERR_NOT_FOUND;
            out.objects.push_back(problem->objects[i]);
        }
        if (!out.objects.empty()) out.fixId = "select";
        sink.out.push_back(std::move(out));
        ++sink.count;
        return APEX_OK;
    } catch (...) { return internal(static_cast<ProblemSink*>(sinkPointer)->host.host.context); }
    void runProblemCheck(const Registration& r, const editor::ProblemContext& context, std::vector<editor::Problem>& out) {
        std::vector<editor::Problem> mine;
        ProblemSink sink{*this, context.doc, mine, r.owner};
        const apex_problem_sink native{sizeof(native), &sink, reportProblem};
        const auto* previous = std::exchange(document, &context.doc);
        apex_status status = APEX_ERR_INTERNAL;
        const bool ran = guardedUi(r.owner, "problems.check", [&] { status = r.problemCheck(r.user, &native); });
        document = previous;
        if (ran && status == APEX_OK) {
            std::ranges::move(mine, std::back_inserter(out));
            return;
        }
        // A broken check reports itself instead of half its findings.
        editor::Problem failed;
        failed.severity = editor::ProblemSeverity::Warning;
        failed.kind = editor::ProblemKind::Plugin;
        failed.source = r.owner;
        failed.text = "Check \"" + r.name + "\" failed" + (lastError.empty() ? std::string{} : ": " + lastError);
        out.push_back(std::move(failed));
    }
    void drawHelpers() {
        helperCache = {};
        helperValid = true;
        helperDocument = document;
        helperState = document ? document->historyState() : 0;
        helperClasses = session ? session->entityClassesRevision() : 0;
        if (helperRegistrations.empty() || !document || !session) return;
        const auto& classes = session->entityClasses();
        constexpr std::size_t kMaxHelperItems = 200000;
        const auto visit = [&](const auto& self, document::NodeId id) -> void {
            if (helperCache.segments.size() + helperCache.labels.size() > kMaxHelperItems) return;
            const auto handle = document->handle(id);
            const auto* node = handle ? document->get(*handle) : nullptr;
            if (!node) return;
            if (std::holds_alternative<Handle<document::Entity>>(*handle) && node->block && !session->nodeHidden(id)) {
                const auto* classname = node->block->find("classname");
                const auto name = classname ? classname->decodedValue().value_or("") : std::string{};
                if (const auto* cls = classes.find(name)) {
                    for (const auto& helper : cls->helpers) {
                        if (!std::holds_alternative<assets::UnknownHelper>(assets::parseHelper(helper))) continue;
                        const auto found = helperRegistrations.find(toLowerAscii(trim(helper.name)));
                        if (found == helperRegistrations.end()) continue;
                        const auto pose = document::entityPose(*node->block);
                        const std::string helperName = toLowerAscii(trim(helper.name));
                        apex_helper_call call{sizeof(call), id, string(cls->name), string(helperName), string(helper.arguments),
                            {pose.origin.x, pose.origin.y, pose.origin.z}, {pose.anglesDeg.x, pose.anglesDeg.y, pose.anglesDeg.z}};
                        struct Out { HelperDrawing& drawing; } out{helperCache};
                        const apex_helper_sink native{sizeof(native), &out,
                            [](void* o, const apex_tool_segment* in) noexcept -> apex_status {
                                try {
                                    auto& drawing = static_cast<Out*>(o)->drawing;
                                    OverlaySegment segment;
                                    if (!in || !segmentOf(*in, segment)) return APEX_ERR_INVALID;
                                    if (drawing.segments.size() >= 200000) return APEX_ERR_STATE;
                                    drawing.segments.push_back(segment);
                                    return APEX_OK;
                                } catch (...) { return APEX_ERR_INTERNAL; }
                            },
                            [](void* o, const apex_overlay_label* in) noexcept -> apex_status {
                                try {
                                    auto& drawing = static_cast<Out*>(o)->drawing;
                                    OverlayLabel label;
                                    if (!in || !labelOf(*in, label)) return APEX_ERR_INVALID;
                                    if (drawing.labels.size() >= 200000) return APEX_ERR_STATE;
                                    drawing.labels.push_back(std::move(label));
                                    return APEX_OK;
                                } catch (...) { return APEX_ERR_INTERNAL; }
                            }};
                        const auto segmentsBefore = helperCache.segments.size(), labelsBefore = helperCache.labels.size();
                        apex_status status = APEX_ERR_INTERNAL;
                        const bool ran = guardedUi(found->second.owner, "helper.draw", [&] { status = found->second.draw(found->second.user, &call, &native); });
                        if (!ran || status != APEX_OK) {
                            helperCache.segments.resize(segmentsBefore);
                            helperCache.labels.resize(labelsBefore);
                        }
                    }
                }
            }
            for (const auto child : node->children) self(self, child);
        };
        for (const auto root : document->roots()) visit(visit, root);
    }
    void note(const std::string& text) {
        if (logger) logger(text); else log::info("plugin: {}", text);
    }
    void commitRegistrations() {
        const auto batch = std::move(pending);
        pending.clear();
        for (const auto& r : batch) commitOne(r);
    }
    void commitOne(const Registration& r) {
        if (r.kind == Registration::Kind::Section) { uiPanels.insert_or_assign(r.id, r); return; }
        if (const auto found = cells.find(r.id); found != cells.end() && reloading) {
            // Same id as before the reload: keep the registry entry, point it at the new callbacks.
            auto& cell = *found->second;
            if (cell.kind != r.kind || cell.owner != r.owner) {
                note("script " + r.owner + ": " + r.id + " changed kind; restart Apex to use it");
                return;
            }
            cell = r;
            if (r.kind == Registration::Kind::Panel && r.uiCallbacks.describe) uiPanels.insert_or_assign(r.id, r);
            return;
        }
        if (reloading) {
            const bool open = r.kind == Registration::Kind::Problem ? !problems->frozen()
                              : r.kind == Registration::Kind::Panel ? true
                              : !editor::actionRegistry().frozen() && (r.kind != Registration::Kind::Tool || !editor::toolRegistry().frozen());
            if (!open) { note("script " + r.owner + ": new " + r.id + " is available after a restart"); return; }
        }
        const auto cell = std::make_shared<Registration>(r);
        cells.emplace(r.id, cell);
        if (r.kind == Registration::Kind::Problem) {
            problems->add(r.id, [this, life = std::weak_ptr(lifetime), cell](const editor::ProblemContext& context, std::vector<editor::Problem>& out) {
                if (!life.expired() && !cell->dead) runProblemCheck(*cell, context, out);
            });
            return;
        }
        if (r.kind == Registration::Kind::Panel) {
            if (panelRegistrar(r.id, r.name) && r.uiCallbacks.describe) uiPanels.emplace(r.id, r);
            return;
        }
        const auto callbackFn = [this, life = std::weak_ptr(lifetime), cell] {
            if (!life.expired() && !cell->dead) callback(cell->owner, "action", cell->first, cell->user);
        };
        editor::Action action{r.id, r.name, "Plugin", r.description};
        action.owner = r.owner;
        action.suggestedBindings = r.suggestions;
        action.suggestedContexts = r.suggestionContexts;
        editor::mutableActionRegistry().add(std::move(action),
                                              r.kind == Registration::Kind::Action ? callbackFn : std::function<void()>{});
        if (r.kind != Registration::Kind::Tool) return;
        auto* selfPtr = this;
        editor::mutableToolRegistry().add(r.id, [selfPtr, life = std::weak_ptr(lifetime), cell](editor::EditorSession& session) -> std::unique_ptr<editor::Tool> {
            if (life.expired() || cell->dead) return std::make_unique<RegisteredTool>(cell->id, nullptr, nullptr);
            const Registration reg = *cell;
            if (reg.extended) {
                auto guard = [selfPtr, life, owner = reg.owner](const char* name, const std::function<void()>& fn) {
                    if (life.expired()) return;
                    Mark mark(selfPtr->breadcrumb, owner, name);
                    const auto previous = std::exchange(selfPtr->callbackOwner, owner);
                    try { fn(); } catch (...) { selfPtr->lastError = "exception in plugin tool callback"; }
                    selfPtr->callbackOwner = previous;
                };
                auto tool = std::make_unique<PluginTool>(session, reg.id, reg.user, reg.callbacks, std::move(guard),
                    [selfPtr, life](const char* message) { if (!life.expired()) selfPtr->setError(message); });
                std::erase_if(selfPtr->toolRetirements, [](const auto& item) { return item.retire.expired(); });
                selfPtr->toolRetirements.push_back({reg.owner, reg.id, tool->retirement(), tool->reviver()});
                return tool;
            }
            auto on = [selfPtr, life, cell] { if (!life.expired() && !cell->dead) selfPtr->callback(cell->owner, "tool.activate", cell->first, cell->user); };
            auto off = [selfPtr, life, cell] { if (!life.expired() && !cell->dead) selfPtr->callback(cell->owner, "tool.deactivate", cell->second, cell->user); };
            return std::make_unique<RegisteredTool>(reg.id, std::move(on), std::move(off));
        });
    }
    // ---- script plugins (0.9.7) ----
    // Takes a script plugin's registrations out of service before its module is unloaded: live tool
    // instances are retired (destroy_instance runs while the old code is still mapped), every callback the
    // script registered goes dead, and what the host keeps per owner outside the registries is dropped.
    void retireScript(const std::string& owner) {
        for (auto& item : toolRetirements)
            if (item.owner == owner) if (auto retire = item.retire.lock()) (*retire)();
        std::erase_if(toolRetirements, [&](const auto& item) { return item.retire.expired(); });
        for (auto& [id, cell] : cells) {
            if (cell->owner != owner) continue;
            cell->dead = true;
            cell->user = nullptr; cell->first = cell->second = nullptr;
            cell->callbacks = {}; cell->uiCallbacks = {}; cell->problemCheck = nullptr;
        }
        std::erase_if(uiPanels, [&](const auto& item) { return item.second.owner == owner; });
        pending.clear();
        forgetOwner(owner);
    }
    static void scriptSaved(void* user, apex_str) noexcept {
        auto* entry = static_cast<ScriptEntry*>(user);
        try { entry->host->reloadEntry(*entry); } catch (...) { entry->host->setError("exception while reloading a script"); }
    }
    void watchScript(ScriptEntry& entry, std::uint64_t signature) {
        const auto& item = plugins[entry.index];
        Watch watch{++lastWatch, item.id, canonical(item.script), &entry, scriptSaved};
        watch.signature = signature;
        watches.push_back(std::move(watch));
    }
    bool loadScript(ScriptEntry& entry, bool reload) {
        auto& item = plugins[entry.index];
        const auto fail = [&](const std::string& why) {
            pending.clear();
            retireScript(item.id);
            if (entry.engine) { try { Mark mark(breadcrumb, item.id, "script.unload"); entry.engine->unload(); } catch (...) { } entry.engine.reset(); }
            item.error = why; item.loaded = false;
            setError(why.c_str());
            note("script " + item.id + ": " + why);
            return false;
        };
        try { entry.engine = scriptFactory ? scriptFactory(item.runtime) : nullptr; }
        catch (...) { return fail("script engine factory threw for runtime \"" + item.runtime + "\""); }
        if (!entry.engine) return fail("no script engine for runtime \"" + item.runtime + "\" in this build");
        pending.clear();
        registering = item.id;
        reloading = reload;
        std::string error;
        bool ok = false;
        try {
            Mark mark(breadcrumb, item.id, reload ? "script.reload" : "script.load");
            const auto previous = std::exchange(callbackOwner, item.id);
            try { ok = entry.engine->load({item.id, item.folder, item.script, item.runtime}, &host, error); }
            catch (const std::exception& e) { ok = false; error = std::string("exception in script engine: ") + e.what(); }
            catch (...) { ok = false; error = "exception in script engine"; }
            callbackOwner = previous;
        } catch (...) { ok = false; error = "script load failed"; }
        registering.clear();
        if (ok) {
            try { commitRegistrations(); }
            catch (const std::exception& e) { ok = false; error = std::string("registration failed: ") + e.what(); }
            catch (...) { ok = false; error = "registration failed"; }
        }
        reloading = false;
        if (!ok) return fail(error.empty() ? "the script failed to load" : error);
        item.error.clear(); item.loaded = true;
        if (reload) reviveTools(item.id);
        return true;
    }
    // Sessions keep their tool objects, so after a reload the retired ones are pointed at the new version:
    // same id, fresh instance. A tool whose id the new version dropped stays retired (inert).
    void reviveTools(const std::string& owner) {
        const auto items = toolRetirements;   // a revive can run plugin code that creates tools
        for (const auto& item : items) {
            if (item.owner != owner) continue;
            const auto found = cells.find(item.tool);
            if (found == cells.end() || found->second->dead || !found->second->extended) continue;
            const auto revive = item.revive.lock();
            if (!revive) continue;
            const Registration reg = *found->second;
            auto guarded = [&] { (*revive)(reg.user, reg.callbacks); };
            Mark mark(breadcrumb, owner, "tool.revive");
            const auto previous = std::exchange(callbackOwner, owner);
            try { guarded(); } catch (...) { lastError = "exception while reviving a script tool"; }
            callbackOwner = previous;
        }
    }
    bool reloadEntry(ScriptEntry& entry) {
        auto& item = plugins[entry.index];
        if (!item.enabled) return false;
        const auto signature = signatureOf(item.script);   // taken before reading, so a save during the load triggers again
        retireScript(item.id);
        if (entry.engine) {
            try { Mark mark(breadcrumb, item.id, "script.unload"); entry.engine->unload(); } catch (...) { }
            entry.engine.reset();
        }
        const bool ok = loadScript(entry, true);
        std::erase_if(watches, [&](const Watch& w) { return w.user == &entry; });
        watchScript(entry, signature);
        if (ok) note("script " + item.id + ": reloaded");
        return ok;
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
        // Jobs first: join workers (and discard completions) while the plugins' code is still mapped.
        {
            std::lock_guard lock(jobMutex);
            jobsClosed = true;
        }
        jobPool.reset();
        // Stop provider calls and wait for the ones in flight (they run plugin code on helper threads). A call
        // that never returns pins its library: it is left mapped rather than unmapped under a running thread.
        bool pinned = false;
        {
            std::vector<std::shared_ptr<ProviderCore>> cores;
            { std::lock_guard lock(vfsMutex); for (auto& r : vfsRegistrations) cores.push_back(r.core); }
            for (auto& core : cores) core->closed.store(true);
            const auto deadline = std::chrono::steady_clock::now() + vfsTimeout + std::chrono::milliseconds(200);
            for (auto& core : cores) {
                while (core->inFlight.load() != 0 && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                if (core->inFlight.load() != 0) pinned = true;
            }
        }
        // Destroy plugin-owned instances while their code and host APIs are live.
        for (const auto& item : toolRetirements) if (auto retire = item.retire.lock()) (*retire)();
        lifetime.reset();
        // Script modules go before the native libraries their engine may be built on.
        for (auto it = scripts.rbegin(); it != scripts.rend(); ++it) {
            if (!(*it)->engine) continue;
            try { Mark mark(breadcrumb, plugins[(*it)->index].id, "script.unload"); (*it)->engine->unload(); } catch (...) { }
            (*it)->engine.reset();
        }
        for (auto it = loaded.rbegin(); it != loaded.rend(); ++it) {
            if (it->plugin->struct_size >= offsetof(apex_plugin, deactivate) + sizeof(it->plugin->deactivate) && it->plugin->deactivate) {
                Mark mark(breadcrumb, plugins[it->index].id, "deactivate");
                try { it->plugin->deactivate(it->plugin->user); } catch (...) { }
            }
            if (!pinned) closeLibrary(it->library);
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
                const auto scriptName = field(parsed->root, "script");
                if (parsed->root.find("script") && scriptName.empty()) throw std::runtime_error("script must be a file name");
                if (!scriptName.empty()) {
                    // A script plugin: `script` (a file inside the folder) and `runtime`, no native `entry`.
                    if (entry) throw std::runtime_error("a plugin has either entry or script, not both");
                    plugin.runtime = field(parsed->root, "runtime");
                    if (plugin.runtime.empty()) throw std::runtime_error("script needs a runtime");
                    const std::filesystem::path relative{scriptName};
                    if (relative.is_absolute() || relative.has_root_name() || relative.has_root_directory() ||
                        std::ranges::any_of(relative, [](const auto& part) { return part == ".."; }))
                        throw std::runtime_error("script must be inside the plugin folder");
                    plugin.script = dir / relative;
                    if (!std::filesystem::is_regular_file(plugin.script)) throw std::runtime_error("script file not found: " + scriptName);
                } else {
                    const auto filename = entry ? field(*entry, platform()) : std::string{};
                    if (filename.empty()) throw std::runtime_error("missing entry for " + platform());
                    plugin.entry = dir / filename;
                }
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
void Host::setScriptEngineFactory(ScriptEngineFactory factory) { impl_->scriptFactory = std::move(factory); }
bool Host::reloadScript(std::string_view id, std::string* message) {
    auto& s = *impl_;
    if (std::this_thread::get_id() != s.mainThread) { if (message) *message = "reload from the editor thread"; return false; }
    for (const auto& entry : s.scripts) {
        if (s.plugins[entry->index].id != id) continue;
        bool ok = false;
        try { ok = s.reloadEntry(*entry); } catch (...) { s.setError("exception while reloading a script"); }
        if (message) *message = s.plugins[entry->index].error;
        return ok;
    }
    if (message) *message = "not a loaded script plugin";
    return false;
}
void Host::activate(bool safeMode) {
    if (safeMode) return;
    for (std::size_t i = 0; i < impl_->plugins.size(); ++i) {
        auto& item = impl_->plugins[i];
        if (!item.enabled || !item.error.empty() || item.loaded) continue;
        if (!item.script.empty()) {
            auto entry = std::make_unique<Impl::ScriptEntry>(Impl::ScriptEntry{impl_.get(), i, nullptr});
            const auto signature = Impl::signatureOf(item.script);
            auto& ref = *entry;
            impl_->scripts.push_back(std::move(entry));
            impl_->loadScript(ref, false);
            impl_->watchScript(ref, signature);   // even after a failure, so fixing the file reloads it
            continue;
        }
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
        if (!item.error.empty()) { impl_->pending.clear(); impl_->forgetOwner(item.id); closeLibrary(lib); continue; }
        impl_->commitRegistrations();
        impl_->loaded.push_back({i, lib, plugin}); item.loaded = true;
    }
}
const std::vector<ImporterInfo>& Host::importers() const { return impl_->importerInfos; }
const ImporterInfo* Host::importerFor(const std::filesystem::path& path) const {
    auto ext = path.extension().string();
    if (ext.starts_with('.')) ext.erase(0, 1);
    ext = toLowerAscii(ext);
    for (const auto& info : impl_->importerInfos)
        if (std::ranges::find(info.extensions, ext) != info.extensions.end()) return &info;
    return nullptr;
}
ImportResult Host::importFile(std::string_view importerId, const std::filesystem::path& path) {
    auto& s = *impl_;
    ImportResult result;
    Impl::ImportSink sink;
    try {
        const auto found = std::ranges::find_if(s.importerEntries, [&](const auto& e) { return e.info.id == importerId; });
        if (found == s.importerEntries.end()) { result.message = "unknown importer"; return result; }
        if (!s.session || !s.session->document()) { result.message = "no map is open"; return result; }
        auto& doc = *s.session->document();
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec)) { result.message = "file not found: " + path.string(); return result; }
        document::NodeId world = 0;
        for (const auto root : doc.roots()) {
            const auto handle = doc.handle(root);
            const auto* node = handle ? doc.get(*handle) : nullptr;
            if (node && node->block && node->block->key() == "world") { world = root; break; }
        }
        if (!world) { result.message = "the map has no world block"; return result; }
        const auto utf8 = path.generic_u8string();
        const std::string text(reinterpret_cast<const char*>(utf8.data()), utf8.size());
        const auto entry = *found;
        s.session->edit("Import " + entry.info.name, [&](document::Transaction& tx) {
            Impl::EditContext context{tx, doc, s};
            const apex_import_context native{sizeof(native), world, &Impl::editOps(), &context, &sink, &Impl::reportImport};
            apex_status status = APEX_ERR_INTERNAL;
            const bool ran = s.guardedUi(entry.info.owner, "import", [&] { status = entry.import(entry.user, string(text), &native); });
            if (!ran) throw std::runtime_error("the importer threw an exception");
            if (context.failure != APEX_OK) throw std::runtime_error("an import operation failed: " + s.lastError);
            if (status != APEX_OK) throw std::runtime_error("the importer failed");
        });
        result.ok = true;
    } catch (const std::exception& e) {
        result.message = e.what();
    } catch (...) { result.message = "import failed"; }
    for (const auto& line : sink.messages) result.message += (result.message.empty() ? "" : "; ") + line;
    return result;
}
std::vector<assets::VfsProviderMount> Host::vfsProviders() const {
    std::lock_guard lock(impl_->vfsMutex);
    std::vector<assets::VfsProviderMount> mounts;
    for (const auto& r : impl_->vfsRegistrations) mounts.push_back({std::make_shared<PluginVfsProvider>(r.core), r.position});
    return mounts;
}
std::vector<VfsProviderInfo> Host::vfsProviderInfos() const {
    std::lock_guard lock(impl_->vfsMutex);
    std::vector<VfsProviderInfo> infos;
    for (const auto& r : impl_->vfsRegistrations)
        infos.push_back({r.id, r.core->name, r.owner, r.core->faulted.load(), static_cast<std::size_t>(r.core->inFlight.load())});
    return infos;
}
void Host::setVfsTimeout(std::chrono::milliseconds timeout) {
    std::lock_guard lock(impl_->vfsMutex);
    impl_->vfsTimeout = timeout;
    for (auto& r : impl_->vfsRegistrations) r.core->timeoutMs.store(timeout.count());
}
void Host::setVfsRefreshHandler(std::function<void(std::string_view)> handler) { impl_->vfsRefreshHandler = std::move(handler); }
void Host::setAssetsBackend(AssetsBackend* backend) { impl_->assetsBackend = backend; }
std::size_t Host::drainJobs() {
    auto& s = *impl_;
    if (std::this_thread::get_id() != s.mainThread) return 0;
    JobPool* pool = nullptr;
    { std::lock_guard lock(s.jobMutex); pool = s.jobPool.get(); }
    return pool ? pool->drainCompletions() : 0;
}
std::size_t Host::pendingJobs() const {
    std::lock_guard lock(impl_->jobMutex);
    return impl_->jobHandles.size();
}
void Host::addWatchRoot(const std::filesystem::path& root) { impl_->watchRoots.push_back(Impl::canonical(root)); }
void Host::setWatchDebounce(std::chrono::milliseconds quiet) { impl_->debounce = quiet; }
bool Host::hasWatches() const { return !impl_->watches.empty(); }
std::size_t Host::pollWatches(std::chrono::steady_clock::time_point now) {
    auto& s = *impl_;
    if (std::this_thread::get_id() != s.mainThread) return 0;
    std::vector<std::uint64_t> ready;
    for (auto& watch : s.watches) {
        std::uint64_t signature = 0;
        try { signature = Impl::signatureOf(watch.path); } catch (...) { continue; }
        if (signature != watch.signature) {
            watch.signature = signature;
            watch.pending = true;
            watch.since = now;   // a burst of writes keeps pushing delivery back
        } else if (watch.pending && now - watch.since >= s.debounce) {
            watch.pending = false;
            ready.push_back(watch.id);
        }
    }
    std::size_t delivered = 0;
    for (const auto id : ready) {
        const auto found = std::ranges::find(s.watches, id, &Impl::Watch::id);
        if (found == s.watches.end()) continue;   // unwatched by an earlier callback
        const auto watch = *found;
        const auto text = watch.path.generic_u8string();
        const std::string path(reinterpret_cast<const char*>(text.data()), text.size());
        s.guardedUi(watch.owner, "watch.changed", [&] { watch.fn(watch.user, string(path)); });
        ++delivered;
    }
    return delivered;
}
void Host::useProblemRegistry(editor::ProblemRegistry& registry) { impl_->problems = &registry; }
const std::vector<OverlayData>& Host::overlays() const { return impl_->overlays; }
std::uint64_t Host::overlayRevision() const { return impl_->overlayRevision; }
bool Host::hasHelpers() const { return !impl_->helperRegistrations.empty(); }
const HelperDrawing& Host::helperDrawing() {
    auto& s = *impl_;
    const auto state = s.document ? s.document->historyState() : 0;
    const auto classes = s.session ? s.session->entityClassesRevision() : 0;
    if (!s.helperValid || s.helperDocument != s.document || s.helperState != state || s.helperClasses != classes) {
        try { s.drawHelpers(); } catch (...) { s.helperCache = {}; s.setError("exception while drawing plugin helpers"); }
    }
    return s.helperCache;
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
            const auto previous = std::exchange(impl_->callbackOwner, impl_->plugins[loaded.index].id);
            try { loaded.plugin->on_document_closing(loaded.plugin->user); } catch (...) { impl_->plugins[loaded.index].error = "exception in on_document_closing"; }
            impl_->callbackOwner = previous;
        }
    }
    impl_->document = document;
    impl_->dropOverlayData();  // Plugin overlays describe one document; the plugin draws again when it opens.
    impl_->helperValid = false;
    for (const auto& loaded : impl_->loaded) {
        if (document && loaded.plugin->struct_size >= offsetof(apex_plugin, on_document_opened) + sizeof(loaded.plugin->on_document_opened) && loaded.plugin->on_document_opened) {
            Mark mark(impl_->breadcrumb, impl_->plugins[loaded.index].id, "on_document_opened");
            const auto previous = std::exchange(impl_->callbackOwner, impl_->plugins[loaded.index].id);
            try { loaded.plugin->on_document_opened(loaded.plugin->user); } catch (...) { impl_->plugins[loaded.index].error = "exception in on_document_opened"; }
            impl_->callbackOwner = previous;
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
