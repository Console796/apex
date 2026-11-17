#include "editor/map_problems.hpp"

#include "core/strings.hpp"
#include "document/cordon.hpp"
#include "document/displacement.hpp"
#include "geometry/displacement.hpp"
#include "geometry/displacement_ops.hpp"
#include "editor/overlay_problems.hpp"
#include "geometry/solid_geometry.hpp"

#include <algorithm>
#include <stdexcept>
#include <cmath>
#include <map>
#include <iterator>
#include <set>
#include <sstream>

namespace apex::editor {
namespace {
using document::NodeId;

std::string value(const kv1::Node& block, std::string_view key) {
    const auto* entry = block.find(key);
    return entry ? entry->decodedValue().value_or("") : "";
}

struct EntityInfo {
    NodeId id = 0;
    std::string classname, name;
    std::optional<DVec3> origin;
    std::vector<NodeId> solids;
    AABB solidBounds;
    bool world = false;
};
struct SolidInfo {
    NodeId id = 0, owner = 0; // owner 0: a solid outside any entity node
    bool world = false;       // belongs to worldspawn or to no entity
    std::optional<AABB> bounds;
    std::string invalid;
};
struct Scan {
    std::vector<EntityInfo> entities;
    std::vector<SolidInfo> solids;
};

Scan scan(const document::Document& doc) {
    Scan result;
    std::map<NodeId, std::size_t> entityIndex;
    const auto visit = [&](const auto& self, NodeId id) -> void {
        const auto h = *doc.handle(id);
        const auto& node = *doc.get(h);
        if (std::holds_alternative<Handle<document::Entity>>(h)) {
            EntityInfo info;
            info.id = id;
            info.classname = value(*node.block, "classname");
            info.name = value(*node.block, "targetname");
            info.world = ciEquals(info.classname, "worldspawn");
            if (const auto* origin = node.block->find("origin"))
                if (const auto parsed = origin->asVec3()) info.origin = DVec3((*parsed)[0], (*parsed)[1], (*parsed)[2]);
            entityIndex[id] = result.entities.size();
            result.entities.push_back(std::move(info));
        } else if (std::holds_alternative<Handle<document::Solid>>(h)) {
            SolidInfo info;
            info.id = id;
            NodeId owner = node.parent;
            while (owner) {
                const auto parent = *doc.handle(owner);
                if (std::holds_alternative<Handle<document::Entity>>(parent)) break;
                owner = doc.get(parent)->parent;
            }
            info.owner = owner;
            if (auto geometry = geometry::solidGeometry(*node.block)) {
                if (!geometry->poly.isValid()) info.invalid = "its faces do not close into a convex solid";
                else if (!(geometry->poly.volume() > 1e-6)) info.invalid = "it has no volume";
                else info.bounds = geometry->poly.bounds();
            } else info.invalid = geometry.error().what();
            if (owner) {
                auto& entity = result.entities[entityIndex.at(owner)];
                entity.solids.push_back(id);
                if (info.bounds) entity.solidBounds.expand(*info.bounds);
            }
            result.solids.push_back(std::move(info));
        }
        for (auto child : node.children) self(self, child);
    };
    for (auto root : doc.roots()) visit(visit, root);
    for (auto& solid : result.solids)
        solid.world = !solid.owner || result.entities[entityIndex.at(solid.owner)].world;
    return result;
}

bool isBrushClass(const ProblemContext& context, const std::string& classname) {
    if (context.classes) if (const auto* cls = context.classes->find(classname)) return cls->type == "SolidClass";
    const auto lower = toLowerAscii(classname);
    if (lower.starts_with("func_instance")) return false;
    return lower.starts_with("func_") || lower.starts_with("trigger_");
}

std::string label(const EntityInfo& e) {
    return e.name.empty() ? e.classname : e.classname + " \"" + e.name + "\"";
}

void invalidSolids(const ProblemContext& context, std::vector<Problem>& out) {
    for (const auto& solid : scan(context.doc).solids)
        if (!solid.invalid.empty())
            out.push_back({ProblemSeverity::Error, ProblemKind::InvalidSolid,
                           "Invalid solid " + std::to_string(solid.id) + ": " + solid.invalid,
                           {solid.world ? solid.id : solid.owner}, "select"});
}

void assetProblems(const ProblemContext& context, std::vector<Problem>& out) {
    if (!context.assets) return;
    static const char* kinds[] = {"material", "model", "sound", "particle", "instance"};
    for (const auto& asset : context.assets->missingAssets) {
        Problem p;
        p.kind = ProblemKind::MissingAsset;
        p.severity = asset.kind == AssetKind::Model || asset.kind == AssetKind::Instance
                         ? ProblemSeverity::Error : ProblemSeverity::Warning;
        p.text = std::string("Missing ") + kinds[static_cast<int>(asset.kind)] + ": " + asset.path;
        std::set<NodeId> seen;
        for (const auto& ref : asset.references) {
            const NodeId id = ref.solid ? ref.solid : ref.entity;
            if (id && seen.insert(id).second) p.objects.push_back(id);
        }
        p.fixId = "select";
        out.push_back(std::move(p));
    }
}

void connectionProblems(const ProblemContext& context, std::vector<Problem>& out) {
    if (!context.assets) return;
    for (const auto& bad : context.assets->invalidConnections)
        out.push_back({ProblemSeverity::Warning, ProblemKind::BrokenConnection,
                       "Connection " + bad.output + " on entity " + std::to_string(bad.entity) + ": " +
                           (bad.issues.empty() ? std::string("invalid") : bad.issues.front().message),
                       {bad.entity}, "select"});
    for (const auto& unknown : context.assets->unknownClasses)
        out.push_back({ProblemSeverity::Warning, ProblemKind::UnknownClass,
                       "Unknown entity class " + unknown.classname, {unknown.entity}, "select"});
}

void duplicateNames(const ProblemContext& context, std::vector<Problem>& out) {
    static const std::set<std::string> unique{"point_template", "env_entity_maker", "logic_branch", "logic_case",
        "logic_compare", "math_counter", "logic_relay", "logic_branch_listener", "logic_timer", "logic_auto"};
    std::map<std::string, std::vector<const EntityInfo*>> byName;
    const auto entities = scan(context.doc).entities;
    for (const auto& e : entities)
        if (!e.name.empty() && unique.contains(toLowerAscii(e.classname))) byName[toLowerAscii(e.name)].push_back(&e);
    for (const auto& [name, group] : byName) {
        std::set<std::string> classes;
        for (const auto* e : group) classes.insert(toLowerAscii(e->classname));
        for (const auto& cls : classes) {
            Problem p{ProblemSeverity::Warning, ProblemKind::DuplicateName, {}, {}, "select"};
            for (const auto* e : group) if (toLowerAscii(e->classname) == cls) p.objects.push_back(e->id);
            if (p.objects.size() < 2) continue;
            p.text = "Duplicate targetname \"" + group.front()->name + "\" on " + std::to_string(p.objects.size()) + " " + cls;
            out.push_back(std::move(p));
        }
    }
}

void instanceProblems(const ProblemContext& context, std::vector<Problem>& out) {
    if (!context.instances) return;
    for (const auto& problem : *context.instances) {
        // An unresolved file is already a missing instance asset; report it once.
        if (problem.kind == InstanceProblem::Kind::Unresolved) continue;
        out.push_back({ProblemSeverity::Error, ProblemKind::InstanceProblem,
                       "Instance " + problem.file + ": " + problem.message, {problem.instance}, "select"});
    }
}

void outOfRange(const ProblemContext& context, std::vector<Problem>& out) {
    const auto limit = context.coordinateLimit;
    const auto beyond = [&](DVec3 v) { return glm::any(glm::greaterThan(glm::abs(v), DVec3{limit})); };
    const auto scanned = scan(context.doc);
    std::set<NodeId> reported;
    for (const auto& e : scanned.entities) {
        if (e.world) continue;
        const bool far = (e.origin && beyond(*e.origin)) ||
                         (!e.solidBounds.empty() && (beyond(e.solidBounds.min) || beyond(e.solidBounds.max)));
        if (far && reported.insert(e.id).second)
            out.push_back({ProblemSeverity::Error, ProblemKind::OutOfRange,
                           label(e) + " is outside the +-" + std::to_string(static_cast<long>(limit)) + " map range",
                           {e.id}, "select"});
    }
    for (const auto& s : scanned.solids)
        if (s.world && s.bounds && (beyond(s.bounds->min) || beyond(s.bounds->max)))
            out.push_back({ProblemSeverity::Error, ProblemKind::OutOfRange,
                           "Solid " + std::to_string(s.id) + " is outside the +-" +
                               std::to_string(static_cast<long>(limit)) + " map range", {s.id}, "select"});
}

void entityShape(const ProblemContext& context, std::vector<Problem>& out) {
    for (const auto& e : scan(context.doc).entities) {
        if (e.world || e.classname.empty()) continue;
        if (e.solids.empty()) {
            if (isBrushClass(context, e.classname))
                out.push_back({ProblemSeverity::Error, ProblemKind::EmptyBrushEntity,
                               label(e) + " is a brush entity with no brushes", {e.id}, "select"});
            else if (!e.origin && !(context.classes && context.classes->find(e.classname) == nullptr))
                out.push_back({ProblemSeverity::Warning, ProblemKind::MissingOrigin,
                               label(e) + " has no origin", {e.id}, "select"});
        }
    }
}

void playerStart(const ProblemContext& context, std::vector<Problem>& out) {
    for (const auto& e : scan(context.doc).entities)
        for (const auto& wanted : context.playerStartClasses) if (ciEquals(e.classname, wanted)) return;
    if (context.playerStartClasses.empty()) return;
    out.push_back({ProblemSeverity::Warning, ProblemKind::MissingPlayerStart,
                   "The map has no " + context.playerStartClasses.front(), {}, ""});
}

void emptyCordons(const ProblemContext& context, std::vector<Problem>& out) {
    const auto cordons = document::readCordons(context.doc);
    if (!cordons.enabled) return;
    const auto scanned = scan(context.doc);
    for (const auto& cordon : cordons.boxes) {
        if (!cordon.active) continue;
        bool holds = false;
        for (const auto& e : scanned.entities) {
            if (e.world) continue;
            if ((e.origin && cordon.box.contains(*e.origin)) ||
                (!e.solidBounds.empty() && cordon.box.intersects(e.solidBounds))) { holds = true; break; }
        }
        for (const auto& s : scanned.solids)
            if (!holds && s.world && s.bounds && cordon.box.intersects(*s.bounds)) holds = true;
        if (!holds)
            out.push_back({ProblemSeverity::Warning, ProblemKind::EmptyCordon,
                           "Cordon \"" + cordon.name + "\" holds nothing", {cordon.id}, "select"});
    }
}
void displacements(const ProblemContext& context, std::vector<Problem>& out) {
    for (const auto& solid : scan(context.doc).solids) {
        const auto handle = context.doc.handle(solid.id);
        const auto* node = handle ? context.doc.get(*handle) : nullptr;
        if (!node || !node->block) continue;
        const auto sides = node->block->findAll("side");
        if (std::ranges::none_of(sides, [](const kv1::Node* side) { return document::isDisplaced(*side); })) continue;
        const auto geometry = geometry::solidGeometry(*node->block);
        const auto* poly = geometry ? &geometry->poly : nullptr;
        for (const auto* side : sides) {
            auto found = displacementProblems(*node->block, *side, poly, solid.world ? solid.id : solid.owner);
            std::ranges::move(found, std::back_inserter(out));
        }
    }
}
// D6: neighbouring displacements whose shared base edge has a gap. Unreadable and non-quad
// displacements are left to the checks above.
// docs/plans/probe-results/structural_0_8_9_p0.md: a structural brush adds the same vis portals whatever
// its size, but only pays for them by hiding things. Flag a world brush that has no side, seen along any
// axis, at least 64 units square: it cannot hide a standing player. Tool brushes (hint, skip, clip...)
// and displacement brushes are exempt; nodraw is a real occluder.
void smallStructural(const ProblemContext& context, std::vector<Problem>& out) {
    constexpr double kMinSide = 64.0;
    for (const auto& solid : scan(context.doc).solids) {
        if (!solid.world || !solid.bounds) continue;
        const auto size = solid.bounds->max - solid.bounds->min;
        const auto hides = [&](int a, int b) { return size[a] >= kMinSide && size[b] >= kMinSide; };
        if (hides(1, 2) || hides(0, 2) || hides(0, 1)) continue;
        const auto handle = context.doc.handle(solid.id);
        const auto* node = handle ? context.doc.get(*handle) : nullptr;
        if (!node || !node->block) continue;
        const auto exempt = [](const kv1::Node* side) {
            if (document::isDisplaced(*side)) return true;
            const auto material = toLowerAscii(value(*side, "material"));
            return material.starts_with("tools/") && material != "tools/toolsnodraw";
        };
        if (std::ranges::any_of(node->block->findAll("side"), exempt)) continue;
        std::ostringstream text;
        text << "Structural solid " << solid.id << " (" << size.x << " x " << size.y << " x " << size.z
             << ") is too small to hide a player but still cuts vis portals; consider To Detail";
        out.push_back({ProblemSeverity::Warning, ProblemKind::SmallStructural, text.str(), {solid.id}, "select"});
    }
}

void unsewnDisplacements(const ProblemContext& context, std::vector<Problem>& out) {
    std::vector<geometry::DisplacementPatch> patches;
    struct Owner { int side; NodeId select; };
    std::vector<Owner> owners;
    for (const auto& solid : scan(context.doc).solids) {
        const auto handle = context.doc.handle(solid.id);
        const auto* node = handle ? context.doc.get(*handle) : nullptr;
        if (!node || !node->block) continue;
        const auto sides = node->block->findAll("side");
        if (std::ranges::none_of(sides, [](const kv1::Node* side) { return document::isDisplaced(*side); })) continue;
        const auto geometry = geometry::solidGeometry(*node->block);
        if (!geometry) continue;
        for (const auto* side : sides) {
            if (!document::isDisplaced(*side)) continue;
            const auto* idNode = side->find("id");
            if (!idNode) continue;
            const auto sideId = idNode->asInt();
            if (!sideId) continue;
            auto disp = document::parseDisplacement(*side->find("dispinfo"));
            if (!disp || disp->problem()) continue;
            const auto face = std::ranges::find(geometry->poly.faces, static_cast<std::uint32_t>(*sideId), &geometry::Face::sideId);
            if (face == geometry->poly.faces.end()) continue;
            const auto corners = geometry::displacementCorners(geometry->poly, *face);
            if (!corners) continue;
            patches.push_back({*corners, std::move(*disp)});
            owners.push_back({static_cast<int>(*sideId), solid.world ? solid.id : solid.owner});
        }
    }
    if (patches.size() < 2) return;
    const auto seams = geometry::displacementSeams(patches);
    if (!seams) return;
    for (const auto& seam : *seams) {
        if (!(seam.gap > kUnsewnDisplacementGap)) continue;
        std::ostringstream gap;
        gap << seam.gap;
        std::vector<NodeId> objects{owners[seam.a].select};
        if (owners[seam.b].select != owners[seam.a].select) objects.push_back(owners[seam.b].select);
        out.push_back({ProblemSeverity::Warning, ProblemKind::DisplacementUnsewn,
                       "Unsewn displacement edge between sides " + std::to_string(owners[seam.a].side) + " and " +
                           std::to_string(owners[seam.b].side) + " (gap " + gap.str() +
                           " units; select both and Sew with the Displacement tool)",
                       objects, "select"});
    }
}
} // namespace

std::vector<Problem> displacementProblems(const kv1::Node& solid, const kv1::Node& side,
                                          const geometry::ConvexPolyhedron* poly, NodeId select) {
    std::vector<Problem> out;
    if (!document::isDisplaced(side)) return out;
    const auto* idNode = side.find("id");
    const auto sideId = idNode ? std::optional<int>(idNode->asInt().value_or(0)) : std::nullopt;
    const auto where = "Displacement on side " + (sideId ? std::to_string(*sideId) : std::string("?")) +
                       " of solid " + value(solid, "id");
    const auto disp = document::parseDisplacement(*side.find("dispinfo"));
    if (!disp) {
        out.push_back({ProblemSeverity::Error, ProblemKind::DisplacementUnreadable,
                       where + " cannot be read: " + disp.error().what(), {select}, "select"});
        return out;
    }
    if (const auto problem = disp->problem()) {
        out.push_back({ProblemSeverity::Error, ProblemKind::DisplacementUnreadable,
                       where + " cannot be read: " + *problem, {select}, "select"});
        return out;
    }
    if (poly && sideId) {
        const auto face = std::ranges::find(poly->faces, static_cast<std::uint32_t>(*sideId), &geometry::Face::sideId);
        if (face != poly->faces.end() && !geometry::displacementCorners(*poly, *face))
            out.push_back({ProblemSeverity::Error, ProblemKind::DisplacementNonQuad,
                           where + " is on a non-quad face (VBSP: \"Trying to create a non-quad displacement!\")", {select}, "select"});
    }
    if (!disp->alphas.empty()) {
        const auto [low, high] = std::ranges::minmax(disp->alphas);
        if (low < 0.0 || high > 255.0) {
            const auto number = [](double value) {
                std::ostringstream text;
                text << value;
                return text.str();
            };
            out.push_back({ProblemSeverity::Warning, ProblemKind::DisplacementAlphaRange,
                           where + " has alpha out of range (" + number(low) + " to " + number(high) +
                               "; 0 to 255 expected)", {select}, "select"});
        }
    }
    return out;
}

void ProblemRegistry::add(std::string name, ProblemCheck check) {
    if (frozen_) throw std::logic_error("problem registry is frozen");
    names_.push_back(std::move(name));
    checks_.push_back(std::move(check));
}

std::vector<Problem> ProblemRegistry::run(const ProblemContext& context) const {
    std::vector<Problem> all;
    for (const auto& check : checks_) check(context, all);
    std::stable_sort(all.begin(), all.end(), [](const Problem& a, const Problem& b) { return a.severity < b.severity; });
    return all;
}

ProblemRegistry ProblemRegistry::builtin() {
    ProblemRegistry registry;
    registry.add("solids", invalidSolids);
    registry.add("assets", assetProblems);
    registry.add("connections", connectionProblems);
    registry.add("names", duplicateNames);
    registry.add("instances", instanceProblems);
    registry.add("range", outOfRange);
    registry.add("entities", entityShape);
    registry.add("player-start", playerStart);
    registry.add("cordons", emptyCordons);
    registry.add("displacements", displacements);
    registry.add("displacement-seams", unsewnDisplacements);
    registry.add("overlays", overlayProblems);
    registry.add("small-structural", smallStructural);
    return registry;
}

std::vector<ProblemGroup> groupProblems(const std::vector<Problem>& problems) {
    std::vector<ProblemGroup> groups;
    for (std::size_t i = 0; i < problems.size(); ++i) {
        const std::string source = problems[i].kind == ProblemKind::Plugin ? problems[i].source : std::string{};
        auto it = std::ranges::find(groups, source, &ProblemGroup::source);
        if (it == groups.end()) it = groups.insert(groups.end(), ProblemGroup{source, {}});
        it->indices.push_back(i);
    }
    // Apex's own group first; plugin groups keep their first-appearance order.
    std::ranges::stable_partition(groups, [](const ProblemGroup& g) { return g.source.empty(); });
    return groups;
}

ProblemRegistry& mutableProblemRegistry() {
    static ProblemRegistry registry = ProblemRegistry::builtin();
    return registry;
}
const ProblemRegistry& problemRegistry() { return mutableProblemRegistry(); }

int countProblems(const std::vector<Problem>& problems, ProblemSeverity severity) {
    return static_cast<int>(std::count_if(problems.begin(), problems.end(),
                                          [&](const Problem& p) { return p.severity == severity; }));
}
const char* severityName(ProblemSeverity severity) {
    return severity == ProblemSeverity::Error ? "Error" : severity == ProblemSeverity::Warning ? "Warning" : "Note";
}

} // namespace apex::editor
