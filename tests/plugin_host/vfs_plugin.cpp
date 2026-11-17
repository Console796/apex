// Test plugin for apex.vfs/1 and apex.assets/1, written against the C++ wrapper. The provider serves two
// materials from memory; APEX_TEST_VFS_MODE switches failure modes at call time, APEX_TEST_VFS_POSITION
// ("first", "last", "before:<anchor>", "after:<anchor>") the mount position at activate.
#include <apex/plugin.hpp>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
const apex_host* host = nullptr;
void log(const std::string& text) { host->log(host->context, 1, apex::plugin::string(text)); }
std::string env(const char* name) { const char* value = std::getenv(name); return value ? value : ""; }

constexpr const char* glass = "\"LightmappedGeneric\"\n{\n\t\"$basetexture\" \"vfsplugin/glass\"\n\t\"$surfaceprop\" \"glass\"\n}\n";
constexpr const char* shared = "\"LightmappedGeneric\"\n{\n\t\"$basetexture\" \"vfsplugin/shared\"\n}\n";

struct Provider : apex::plugin::FileProvider {
    std::mutex mutex;
    std::map<std::string, std::string> files{{"materials/vfsplugin/glass.vmt", glass}, {"materials/shared.vmt", shared}};
    void sleepIf(const char* mode) {
        if (env("APEX_TEST_VFS_MODE") != mode) return;
        const auto ms = std::atoi(env("APEX_TEST_VFS_SLEEP_MS").c_str());
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
    apex_status refresh() override {
        if (env("APEX_TEST_VFS_MODE") == "throw_refresh") throw std::runtime_error("refresh failure");
        return APEX_OK;
    }
    apex_status list(const std::function<void(std::string_view)>& add) override {
        if (env("APEX_TEST_VFS_MODE") == "throw_list") throw std::runtime_error("list failure");
        sleepIf("slow_list");
        std::lock_guard lock(mutex);
        // APEX_TEST_VFS_VTF: a texture file the provider serves as materials/vfsplugin/glass.vtf.
        if (const auto vtf = env("APEX_TEST_VFS_VTF"); !vtf.empty() && !files.contains("materials/vfsplugin/glass.vtf")) {
            std::ifstream in(vtf, std::ios::binary);
            files["materials/vfsplugin/glass.vtf"] = std::string(std::istreambuf_iterator<char>(in), {});
        }
        for (const auto& [path, bytes] : files) add(path);
        return APEX_OK;
    }
    apex_status open(std::string_view path, std::vector<char>& bytes) override {
        if (env("APEX_TEST_VFS_MODE") == "throw_open") throw std::runtime_error("open failure");
        sleepIf("slow_open");
        std::lock_guard lock(mutex);
        const auto found = files.find(std::string(path));
        if (found == files.end()) return APEX_ERR_NOT_FOUND;
        bytes.assign(found->second.begin(), found->second.end());
        return APEX_OK;
    }
} provider;

void addFile(void*) {
    { std::lock_guard lock(provider.mutex); provider.files["materials/vfsplugin/added.vmt"] = shared; }
    log("add_file");
}
void refreshFiles(void*) { log("refresh=" + std::to_string(apex::plugin::requestVfsRefresh(*host, "plugin.vfs.files"))); }
void refreshOther(void*) { log("refresh_other=" + std::to_string(apex::plugin::requestVfsRefresh(*host, "plugin.other.files"))); }
void lateRegister(void*) {
    log("late_register=" + std::to_string(apex::plugin::registerVfsProvider(*host, "plugin.vfs.late", "Late", provider)));
}
void findMaterial(void*) {
    apex_material_info info{};
    const auto status = apex::plugin::findMaterial(*host, env("APEX_TEST_VFS_MATERIAL"), info);
    log("find=" + std::to_string(status) + ":" + std::string(apex::plugin::view(info.name)) + ":" +
        std::string(apex::plugin::view(info.base_texture)) + ":" + std::string(apex::plugin::view(info.surface_prop)) + ":" +
        std::string(apex::plugin::view(info.source)));
}
void listMaterials(void*) {
    size_t count = 0, ours = 0;
    const auto status = apex::plugin::listMaterials(*host, [&](const apex_material_info& info) {
        ++count;
        if (apex::plugin::view(info.name).starts_with("vfsplugin/")) ++ours;
        return true;
    });
    log("list=" + std::to_string(status) + ":" + std::to_string(ours) + "/" + std::to_string(count));
}
void writeVmt(void*) {
    std::vector<apex::plugin::VmtEdit> edits{{"$surfaceprop", env("APEX_TEST_VFS_VALUE"), false}, {"$comment", "", true}};
    log("write=" + std::to_string(apex::plugin::writeVmt(*host, env("APEX_TEST_VFS_MATERIAL"), edits)) + ":" +
        std::string(apex::plugin::view(host->last_error(host->context))));
}
void writeBad(void*) {
    std::vector<apex::plugin::VmtEdit> bad{{"bad key", "x", false}};
    log("write_bad=" + std::to_string(apex::plugin::writeVmt(*host, "anything", bad)));
    log("write_none=" + std::to_string(apex::plugin::writeVmt(*host, "anything", {})));
}

apex_status activate(void*) {
    using namespace apex::plugin;
    const auto position = env("APEX_TEST_VFS_POSITION");
    uint32_t kind = 1;
    std::string anchor;
    if (position == "first") kind = 0;
    else if (position.starts_with("before:")) { kind = 2; anchor = position.substr(7); }
    else if (position.starts_with("after:")) { kind = 3; anchor = position.substr(6); }
    log("register=" + std::to_string(registerVfsProvider(*host, "plugin.vfs.files", "Test files", provider, kind, anchor)));
    log("duplicate=" + std::to_string(registerVfsProvider(*host, "plugin.vfs.files", "Again", provider)));
    log("foreign=" + std::to_string(registerVfsProvider(*host, "plugin.other.files", "Foreign", provider)));
    const auto* registry = actions(*host);
    if (!registry) return APEX_ERR_UNSUPPORTED;
    const auto add = [&](const char* id, void (*fn)(void*)) {
        registry->register_action(host->context, string(id), string(id), string("test"), nullptr, fn, nullptr, 0);
    };
    add("plugin.vfs.add_file", addFile);
    add("plugin.vfs.refresh", refreshFiles);
    add("plugin.vfs.refresh_other", refreshOther);
    add("plugin.vfs.late_register", lateRegister);
    add("plugin.vfs.find", findMaterial);
    add("plugin.vfs.list", listMaterials);
    add("plugin.vfs.write", writeVmt);
    add("plugin.vfs.write_bad", writeBad);
    return APEX_OK;
}
const apex_plugin plugin{sizeof(apex_plugin), APEX_PLUGIN_ABI_MAJOR, APEX_PLUGIN_ABI_MINOR, nullptr, activate, nullptr, nullptr, nullptr, nullptr};
}
extern "C" APEX_PLUGIN_EXPORT const apex_plugin* apex_plugin_entry(const apex_host* h) { host = h; return &plugin; }
