#pragma once
#include <functional>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <memory>
namespace apex::document { class Document; }
namespace apex::plugin_host {
struct Plugin { std::string id, name, version, error; std::filesystem::path folder, entry; bool enabled = true, loaded = false; };
bool inNamespace(std::string_view plugin, std::string_view id);
class Host {
public:
    Host(std::filesystem::path state, std::filesystem::path breadcrumb,
         std::function<void(std::string_view)> log = {});
    ~Host();
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;
    void discover(const std::vector<std::filesystem::path>& paths);
    void activate(bool safeMode = false);
    void setDocument(const document::Document* document);
    void setEnabled(std::string_view id, bool enabled);
    const std::vector<Plugin>& plugins() const;
    std::string staleBreadcrumb() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
