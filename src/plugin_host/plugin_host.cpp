#include "plugin_host/plugin_host.hpp"
#include "core/kv3.hpp"
#include "core/log.hpp"
#include "document/document.hpp"
#include <apex/apex_plugin.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>
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

struct Host::Impl {
    struct Loaded { std::size_t index; Library library; const apex_plugin* plugin; };
    std::filesystem::path state;
    Breadcrumb breadcrumb;
    std::function<void(std::string_view)> logger;
    std::vector<Plugin> plugins;
    std::vector<Loaded> loaded;
    std::set<std::string> disabled;
    const document::Document* document = nullptr;
    std::string scratch, lastError;
    apex_host host{};
    apex_document_v1 documentApi{};
    Impl(std::filesystem::path s, std::filesystem::path b, std::function<void(std::string_view)> log)
        : state(std::move(s)), breadcrumb(b), logger(std::move(log)) {
        std::ifstream file(state);
        for (std::string line; std::getline(file, line);) if (!line.empty()) disabled.insert(line);
        host = {sizeof(host), APEX_PLUGIN_ABI_MAJOR, APEX_PLUGIN_ABI_MINOR, this, interface, writeLog, error};
        documentApi = {sizeof(documentApi), info, rootAt, node, childAt, keyAt};
    }
    static Impl& self(void* context) { return *static_cast<Impl*>(context); }
    static const void* interface(void* context, apex_str name) {
        return view(name) == "apex.document/1" ? &self(context).documentApi : nullptr;
    }
    static void writeLog(void* context, int, apex_str message) {
        auto& s = self(context);
        if (s.logger) s.logger(view(message)); else log::info("plugin: {}", view(message));
    }
    static apex_str error(void* context) { return string(self(context).lastError); }
    static const document::Node* find(Impl& s, apex_node_id id) {
        if (!s.document) return nullptr;
        auto handle = s.document->handle(id);
        return handle ? s.document->get(*handle) : nullptr;
    }
    static apex_status info(void* context, apex_document_info* out) {
        if (!out || out->struct_size < sizeof(*out)) return APEX_ERR_INVALID;
        const auto* doc = self(context).document;
        out->is_open = doc != nullptr;
        out->revision = doc ? doc->historyState() : 0;
        out->root_count = doc ? doc->roots().size() : 0;
        return APEX_OK;
    }
    static apex_status rootAt(void* context, size_t index, apex_node_id* out) {
        const auto* doc = self(context).document;
        if (!out) return APEX_ERR_INVALID;
        if (!doc) return APEX_ERR_STATE;
        if (index >= doc->roots().size()) return APEX_ERR_NOT_FOUND;
        *out = doc->roots()[index]; return APEX_OK;
    }
    static apex_status node(void* context, apex_node_id id, apex_node_info* out) {
        if (!out || out->struct_size < sizeof(*out)) return APEX_ERR_INVALID;
        auto& s = self(context); const auto* n = find(s, id);
        if (!n) return s.document ? APEX_ERR_NOT_FOUND : APEX_ERR_STATE;
        out->id = n->id; out->parent = n->parent; out->kind = string(n->block->key());
        out->child_count = n->children.size(); out->key_count = 0;
        for (const auto& child : n->block->children()) if (child.kind() == kv1::NodeKind::Value) ++out->key_count;
        return APEX_OK;
    }
    static apex_status childAt(void* context, apex_node_id id, size_t index, apex_node_id* out) {
        if (!out) return APEX_ERR_INVALID;
        auto& s = self(context); const auto* n = find(s, id);
        if (!n) return s.document ? APEX_ERR_NOT_FOUND : APEX_ERR_STATE;
        if (index >= n->children.size()) return APEX_ERR_NOT_FOUND;
        *out = n->children[index]; return APEX_OK;
    }
    static apex_status keyAt(void* context, apex_node_id id, size_t index, apex_str* key, apex_str* value) {
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
    }
    void save() {
        if (!state.parent_path().empty()) std::filesystem::create_directories(state.parent_path());
        std::ofstream file(state, std::ios::trunc);
        for (const auto& id : disabled) file << id << '\n';
        if (!file) throw std::runtime_error("cannot save plugin enabled state");
    }
    ~Impl() {
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
        try {
            if (plugin->activate) { Mark mark(impl_->breadcrumb, item.id, "activate"); if (plugin->activate(plugin->user) != APEX_OK) item.error = "activation failed"; }
        } catch (...) { item.error = "exception in activate"; }
        if (!item.error.empty()) { closeLibrary(lib); continue; }
        impl_->loaded.push_back({i, lib, plugin}); item.loaded = true;
    }
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
