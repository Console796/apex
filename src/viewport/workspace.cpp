#include "viewport/workspace.hpp"

#include <algorithm>

namespace apex::viewport {
namespace {

Error bad(std::string message) { return Error{ErrorCode::InvalidArgument, std::move(message)}; }

// Turns a single-pane `pane` into Camera | Top over Front | Side (pane order Camera, Top, Front, Side).
void makeQuad(ViewportLayout& layout, NodeId camera) {
    const NodeId bottomLeft = *layout.split(camera, Orientation::Vertical);
    const NodeId topRight = *layout.split(camera, Orientation::Horizontal);
    const NodeId bottomRight = *layout.split(bottomLeft, Orientation::Horizontal);
    applyPreset(*layout.viewport(camera), Preset::Camera);
    applyPreset(*layout.viewport(topRight), Preset::Top);
    applyPreset(*layout.viewport(bottomLeft), Preset::Front);
    applyPreset(*layout.viewport(bottomRight), Preset::Side);
}

ViewportLayout layoutTemplate() {
    ViewportLayout layout(makePreset(Preset::Camera));
    const NodeId top = layout.paneIds().front();
    const NodeId log = *layout.split(top, Orientation::Vertical, 0.8);
    const NodeId views = *layout.split(top, Orientation::Horizontal, 0.2);
    const NodeId inspector = *layout.split(top, Orientation::Vertical, 0.55);
    (void)layout.setRole(top, AreaRole::Outliner);
    (void)layout.setRole(inspector, AreaRole::Inspector);
    const NodeId visgroups = *layout.split(log, Orientation::Horizontal, 0.6);
    const NodeId history = *layout.split(visgroups, Orientation::Horizontal, 0.5);
    (void)layout.setRole(log, AreaRole::Log);
    (void)layout.setRole(visgroups, AreaRole::Visgroups);
    (void)layout.setRole(history, AreaRole::History);
    makeQuad(layout, views);
    (void)layout.setActive(views);
    return layout;
}

ViewportLayout modelingTemplate() {
    ViewportLayout layout(makePreset(Preset::Camera));
    const NodeId camera = layout.paneIds().front();
    const NodeId side = *layout.split(camera, Orientation::Horizontal, 0.78);
    const NodeId inspector = *layout.split(side, Orientation::Vertical, 0.4);
    (void)layout.setRole(side, AreaRole::Outliner);
    (void)layout.setRole(inspector, AreaRole::Inspector);
    (void)layout.setActive(camera);
    return layout;
}

ViewportLayout fourViewsTemplate() {
    ViewportLayout layout(makePreset(Preset::Camera));
    const NodeId views = layout.paneIds().front();
    const NodeId outliner = *layout.split(views, Orientation::Horizontal, 0.82);
    (void)layout.setRole(outliner, AreaRole::Outliner);
    (void)layout.setCollapsed(outliner, true);
    makeQuad(layout, views);
    (void)layout.setActive(views);
    return layout;
}

json::Value str(std::string v) { json::Value r; r.data = std::move(v); return r; }
json::Value num(double v) { json::Value r; r.data = v; return r; }

} // namespace

std::vector<std::string_view> workspaceTemplateNames() { return {"Layout", "Modeling", "Four Views"}; }

ViewportLayout workspaceTemplate(std::string_view name) {
    if (name == "Modeling") return modelingTemplate();
    if (name == "Four Views") return fourViewsTemplate();
    return layoutTemplate();
}

WorkspaceSet WorkspaceSet::defaults() {
    WorkspaceSet set;
    for (const auto name : workspaceTemplateNames())
        set.workspaces.push_back({std::string(name), workspaceTemplate(name), std::string(name)});
    return set;
}

std::string WorkspaceSet::uniqueName(std::string_view base) const {
    const auto taken = [&](const std::string& n) {
        return std::ranges::any_of(workspaces, [&](const Workspace& w) { return w.name == n; });
    };
    std::string name(base);
    for (int i = 2; taken(name); ++i) name = std::string(base) + " " + std::to_string(i);
    return name;
}

Result<void> WorkspaceSet::remove(std::size_t index) {
    if (index >= workspaces.size()) return std::unexpected(bad("remove: no such workspace"));
    if (workspaces.size() == 1) return std::unexpected(bad("remove: the last workspace cannot be removed"));
    workspaces.erase(workspaces.begin() + static_cast<std::ptrdiff_t>(index));
    if (active > index || active >= workspaces.size()) --active;
    return {};
}

Result<void> WorkspaceSet::move(std::size_t from, std::size_t to) {
    if (from >= workspaces.size() || to >= workspaces.size()) return std::unexpected(bad("move: no such workspace"));
    const bool wasActive = active == from;
    auto item = std::move(workspaces[from]);
    workspaces.erase(workspaces.begin() + static_cast<std::ptrdiff_t>(from));
    workspaces.insert(workspaces.begin() + static_cast<std::ptrdiff_t>(to), std::move(item));
    if (wasActive) active = to;
    else if (from < active && to >= active) --active;
    else if (from > active && to <= active) ++active;
    return {};
}

json::Value WorkspaceSet::toJson() const {
    json::Value::Array list;
    for (const auto& w : workspaces) {
        json::Value::Object o;
        o["name"] = str(w.name);
        o["layout"] = w.layout.toJson();
        o["template"] = str(w.base);
        json::Value v;
        v.data = std::move(o);
        list.push_back(std::move(v));
    }
    json::Value::Object o;
    o["version"] = num(1);
    o["active"] = num(static_cast<double>(active));
    json::Value arr;
    arr.data = std::move(list);
    o["workspaces"] = std::move(arr);
    json::Value v;
    v.data = std::move(o);
    return v;
}

Result<WorkspaceSet> WorkspaceSet::fromJson(const json::Value& value) {
    const auto fail = [](std::string m) {
        return std::unexpected(Error{ErrorCode::ParseError, "workspaces: " + std::move(m)});
    };
    const auto* top = value.get<json::Value::Object>();
    if (!top) return fail("expected an object");
    const auto it = top->find("workspaces");
    const auto* list = it == top->end() ? nullptr : it->second.get<json::Value::Array>();
    if (!list || list->empty()) return fail("missing or empty 'workspaces'");
    WorkspaceSet set;
    for (const auto& item : *list) {
        const auto* o = item.get<json::Value::Object>();
        if (!o) return fail("bad workspace");
        const auto name = o->find("name");
        const auto* text = name == o->end() ? nullptr : name->second.get<std::string>();
        const auto layout = o->find("layout");
        if (!text || text->empty() || layout == o->end()) return fail("workspace without a name or layout");
        auto parsed = ViewportLayout::fromJson(layout->second);
        if (!parsed) return std::unexpected(parsed.error());
        Workspace w{*text, std::move(*parsed)};
        if (const auto base = o->find("template"); base != o->end())
            if (const auto* b = base->second.get<std::string>()) w.base = *b;
        set.workspaces.push_back(std::move(w));
    }
    if (const auto a = top->find("active"); a != top->end()) {
        const auto* d = a->second.get<double>();
        if (d && *d >= 0 && *d < static_cast<double>(set.workspaces.size())) set.active = static_cast<std::size_t>(*d);
    }
    return set;
}

} // namespace apex::viewport
