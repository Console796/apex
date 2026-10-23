#pragma once

// Workspaces (0.2.12): named area layouts the user switches between, as in Blender's workspace
// tabs. Each holds a whole ViewportLayout, so its areas, roles, split ratios and cameras are its own.

#include "core/json.hpp"
#include "core/result.hpp"
#include "viewport/layout.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace apex::viewport {

struct Workspace {
    std::string name;
    ViewportLayout layout;
    std::string base = "Layout"; // The template it started from: what "reset" goes back to.
};

// Built-in starting points: "Layout" (outliner and inspector beside a quad, log, visgroups and
// history below), "Modeling" (one camera view and an inspector column) and "Four Views" (a Hammer
// quad with the side areas collapsed).
std::vector<std::string_view> workspaceTemplateNames();
ViewportLayout workspaceTemplate(std::string_view name); // Unknown names give "Layout".

struct WorkspaceSet {
    std::vector<Workspace> workspaces;
    std::size_t active = 0;

    static WorkspaceSet defaults(); // One workspace per template.

    // A name not used yet: `base`, then `base 2`, `base 3`, ...
    std::string uniqueName(std::string_view base) const;
    // Removes a workspace; the last one cannot be removed. The active index follows.
    Result<void> remove(std::size_t index);
    Result<void> move(std::size_t from, std::size_t to);

    json::Value toJson() const;
    static Result<WorkspaceSet> fromJson(const json::Value& value);
};

} // namespace apex::viewport
