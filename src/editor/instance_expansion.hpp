#pragma once

#include "document/instance_contents.hpp"
#include "editor/instance_templates.hpp"

#include <set>

namespace apex::editor {

struct InstanceProblem {
    enum class Kind { Unresolved, Cycle, DepthLimit, Load, Reload };
    Kind kind;
    document::NodeId instance;
    std::filesystem::path path;
    std::string file;
    std::string message;
};
struct InstanceReload {
    std::vector<document::NodeId> users;
    std::optional<InstanceProblem> problem;
};

class InstanceExpansion {
public:
    explicit InstanceExpansion(InstanceTemplates& templates) : templates_(templates) {}
    void update(const document::Document& document, const std::filesystem::path& map,
                const std::optional<std::filesystem::path>& extraRoot = {});
    // Call update after invalidation. Failed reloads keep the previous immutable template.
    InstanceReload invalidate(const std::filesystem::path& path);
    const std::vector<document::InstanceContent>& contents() const { return contents_; }
    const std::vector<InstanceProblem>& problems() const { return problems_; }
    const std::map<std::filesystem::path, std::set<document::NodeId>>& dependencies() const { return dependencies_; }

private:
    InstanceTemplates& templates_;
    using Key = std::pair<std::shared_ptr<const document::Document>, std::string>;
    std::map<Key, std::shared_ptr<const document::Document>> copies_;
    std::vector<document::InstanceContent> contents_;
    std::vector<InstanceProblem> problems_;
    std::map<std::filesystem::path, std::set<document::NodeId>> dependencies_;
    std::map<std::filesystem::path, InstanceProblem> reloadProblems_;
};

} // namespace apex::editor
