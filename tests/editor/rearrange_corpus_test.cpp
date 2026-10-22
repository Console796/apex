#include "corpus.hpp"
#include "editor/editor_session.hpp"
#include "editor/group_ops.hpp"

#include <iostream>
#include <set>

using namespace editor_test;

namespace {

using Identity = std::pair<std::string, int>;
using FieldPath = std::vector<std::string>;
using Allowed = std::set<FieldPath>;

std::string text(const kv1::Document& doc, const kv1::Node& node) {
    const auto span = node.sourceSpan();
    return std::string(doc.source().substr(span.offset, span.length));
}

struct Blocks {
    std::map<Identity, const kv1::Node*> ids;
    std::map<Identity, std::vector<std::string>> parents;
    std::multiset<std::string> hidden;
};

Blocks blocks(const kv1::Document& doc) {
    Blocks result;
    const auto walk = [&](const auto& self, const kv1::Node& node, std::vector<std::string> parents) -> void {
        if (node.kind() != kv1::NodeKind::Block) return;
        std::string location(node.key());
        if (const auto* key = node.find("id")) {
            const auto number = key->asInt();
            REQUIRE(number);
            const Identity identity{std::string(node.key()), *number};
            REQUIRE(result.ids.emplace(identity, &node).second);
            result.parents.emplace(identity, parents);
            location += ':' + std::to_string(*number);
        }
        if (node.key() == "hidden") result.hidden.insert(text(doc, node));
        parents.push_back(std::move(location));
        for (const auto& child : node.children()) self(self, child, parents);
    };
    walk(walk, doc.root(), {});
    return result;
}

// Compare maximal protected subtrees as raw source bytes, including unknown fields,
// duplicate keys, conditions and spelling. Only the explicitly named paths may differ.
std::vector<std::string> protectedText(const kv1::Document& doc, const kv1::Node& node,
                                     const Allowed& allowed, FieldPath path = {}) {
    if (allowed.contains(path)) return {};
    const bool descend = std::ranges::any_of(allowed, [&](const auto& field) {
        return field.size() > path.size() && std::equal(path.begin(), path.end(), field.begin());
    });
    if (!descend) return {text(doc, node)};
    std::vector<std::string> result{std::string(node.rawKey()), std::string(node.condition())};
    for (const auto& child : node.children()) {
        auto childPath = path;
        childPath.emplace_back(child.key());
        auto values = protectedText(doc, child, allowed, std::move(childPath));
        result.insert(result.end(), values.begin(), values.end());
    }
    return result;
}

// A newly created clipboard copy can have fresh indentation. Check its complete field
// tree and raw token bodies, allowing only fresh identity and group membership values.
std::vector<std::string> cloneFields(const kv1::Node& node, FieldPath path = {}) {
    if (path == FieldPath{"id"} || path == FieldPath{"side", "id"} ||
        path == FieldPath{"editor", "groupid"}) return {};
    std::vector<std::string> result{std::to_string(static_cast<int>(node.kind())),
                                  std::string(node.rawKey()), std::string(node.condition()),
                                  std::string(node.rawValue().value_or(""))};
    for (const auto& child : node.children()) {
        auto childPath = path;
        childPath.emplace_back(child.key());
        auto fields = cloneFields(child, std::move(childPath));
        result.insert(result.end(), fields.begin(), fields.end());
    }
    result.emplace_back("end");
    return result;
}

struct Candidates {
    std::vector<NodeId> solids, entities;
    bool groups = false, hidden = false, duplicateVisgroup = false;
};

Candidates candidates(const Document& doc) {
    Candidates result;
    GroupIndex groups(doc);
    for (const auto h : nodes(doc)) {
        const auto* n = doc.get(h);
        result.groups |= std::holds_alternative<Handle<Group>>(h);
        result.hidden |= n->block->key() == "hidden";
        const auto scan = [&](const auto& self, const kv1::Node& b) -> void {
            if (b.key() == "editor" && b.findAll("visgroupid").size() > 1) result.duplicateVisgroup = true;
            for (const auto& c : b.children()) self(self, c);
        };
        scan(scan, *n->block);
        if (!isVisible(doc, n->id)) continue;
        if (std::holds_alternative<Handle<Solid>>(h) && !brushEntityOf(doc, n->id)) {
            const auto geometry = geometry::solidGeometry(*n->block);
            const auto group = groups.parentGroup(n->id);
            const auto sides = n->block->findAll("side");
            const bool displacement = std::ranges::any_of(sides, [](const auto* side) { return side->find("dispinfo"); });
            // No cascading empty-group deletion, displacement editing or redundant planes.
            if (!displacement && geometry && geometry->sides.size() == geometry->poly.faces.size() &&
                (!group || groups.members(*group).size() > 1)) result.solids.push_back(n->id);
        }
        if (std::holds_alternative<Handle<Entity>>(h) && n->block->key() != "world" &&
            solidsOf(doc, n->id).empty() && n->block->find("origin") &&
            n->block->findAll("targetname").size() == 1) result.entities.push_back(n->id);
    }
    const auto sortIds = [&](auto& list) {
        std::ranges::sort(list, [&](NodeId a, NodeId b) { return vmfId(doc, a) < vmfId(doc, b); });
    };
    sortIds(result.solids);
    sortIds(result.entities);
    return result;
}

void record(const Document& doc, std::string_view operation, NodeId node) {
    std::cout << "  " << operation << ": NodeId=" << node << " vmf="
              << doc.get(*doc.handle(node))->block->key() << ':' << vmfId(doc, node).value() << '\n';
}

DVec3 rearranged(DVec3 p) { return {-p.y + 32.0, p.x + 16.0, p.z + 8.0}; }

} // namespace

TEST_CASE("[corpus] rearrangement exit preserves untouched VMF objects by id") {
    const auto paths = corpusPaths();
    if (paths.empty()) {
        MESSAGE("APEX_P2CE_DIR is unset; skipping rearrangement corpus test");
        return;
    }
    // First three eligible sorted paths, plus the first eligible hidden and duplicate-key
    // maps (unique). P2CE: chamber_hallways/entry_hallway_tall_dirty_nocaps_bustfloor,
    // choreo/glados_gen_actor, community/elevators/logic/elevator_departure_down_logic,
    // community/elevators/logic/elevator_departure_up_logic, p2editor/elevator_entrance
    // (all under instances/, .vmf). Eligibility requires groups, two editable world solids
    // and a point entity with an inspector-editable targetname. Use classic viewsettings:
    // srctools rejects some corpus Strata `views` positions (e.g. 16384-axis layouts).
    // Preserve that metadata rather than rewriting it to satisfy an external parser.
    // Fail if coverage disappears.
    std::set<std::filesystem::path> selected;
    bool hidden = false, duplicate = false;
    for (const auto& path : paths) {
        const auto file = readCorpus(path);
        const auto* settings = file.kvDocument().root().find("viewsettings");
        if (settings && settings->find("views")) continue;
        const Document doc(file);
        const auto c = candidates(doc);
        if (!c.groups || c.solids.size() < 2 || c.entities.empty()) continue;
        if (selected.size() < 3 || (!hidden && c.hidden) || (!duplicate && c.duplicateVisgroup)) {
            selected.insert(path);
            hidden |= c.hidden;
            duplicate |= c.duplicateVisgroup;
        }
        if (selected.size() == 5 && hidden && duplicate) break;
    }
    REQUIRE(selected.size() == 5);
    REQUIRE(hidden);
    REQUIRE(duplicate);

    for (const auto& path : selected) {
        CAPTURE(path.string());
        std::cout << "Rearrange " << path << '\n';
        const auto original = readCorpus(path);
        Document doc(original);
        const auto c = candidates(doc);
        const auto solid = c.solids[0], deleted = c.solids[1], entity = c.entities[0];
        const int solidId = *vmfId(doc, solid), deletedId = *vmfId(doc, deleted), entityId = *vmfId(doc, entity);
        const auto beforeSolid = geometry::solidGeometry(*doc.get(*doc.handle(solid))->block).value();
        const auto beforeEntity = *doc.get(*doc.handle(entity))->block;
        const auto origin = beforeEntity.find("origin")->asVec3().value();
        const auto angles = beforeEntity.find("angles") ? beforeEntity.find("angles")->asVec3().value() : kv1::Vec3{};
        const auto oldName = beforeEntity.find("targetname")->decodedValue().value();
        const std::string newName = oldName + "_apex_0210";
        const auto deletedBlock = doc.exportSubtree(deleted);
        const GroupIndex originalGroups(doc);
        const auto solidGroup = originalGroups.parentGroup(solid), entityGroup = originalGroups.parentGroup(entity);
        const auto parentGroupId = solidGroup && solidGroup == entityGroup ? vmfId(doc, *solidGroup) : std::nullopt;

        // Audit actual ChangeSets separately from the expected allowlist below. This must
        // never determine which original objects the preservation assertions exempt.
        std::ostringstream audit;
        std::map<NodeHandle, std::string> identities;
        const auto describe = [&](NodeHandle h) {
            const auto* n = doc.get(h);
            std::ostringstream line;
            line << "NodeId=" << n->id << " " << n->block->key() << " vmf_id=";
            if (const auto number = vmfId(doc, n->id)) line << *number;
            else line << "none";
            if (std::holds_alternative<Handle<Solid>>(h)) {
                line << " sides=";
                for (const auto* side : n->block->findAll("side")) line << side->find("id")->asInt().value() << ',';
            }
            return line.str();
        };
        for (const auto h : nodes(doc)) identities[h] = describe(h);
        const auto observer = doc.observe([&](const ChangeSet& changes) {
            audit << doc.undoLabels().back() << '\n';
            for (const auto h : changes.created) {
                identities[h] = describe(h);
                audit << "  create " << identities.at(h) << '\n';
            }
            for (const auto& change : changes.modified) audit << "  modify " << describe(change.handle) << '\n';
            for (const auto h : changes.destroyed) {
                audit << "  destroy " << identities.at(h) << '\n';
                identities.erase(h);
            }
        });

        EditorSession session;
        session.attach(doc);
        session.selection().setMode(SelectionMode::Objects);
        session.selection().setIgnoreGroups(true);
        session.setTextureLock(false); // Preserve UV keys; this replay changes geometry only.
        session.setPivotMode(PivotMode::Custom);
        session.setCustomPivot({0, 0, 0});
        session.selection().select(solid);
        session.selection().add(entity);
        record(doc, "move+rotate+group+hide", solid);
        record(doc, "move+rotate+group+inspector", entity);
        REQUIRE(session.moveSelection({16, -32, 8}, true));
        REQUIRE(session.rotateSelection({0, 0, 90}));

        session.selection().select(entity);
        const auto model = session.inspectorModel();
        const auto row = std::ranges::find(model.rows(), "targetname", &InspectorModel::Row::key);
        REQUIRE(row != model.rows().end());
        REQUIRE(session.editProperty(static_cast<std::size_t>(row - model.rows().begin()), newName));

        session.selection().select(solid);
        REQUIRE(session.duplicateSelection());
        REQUIRE(session.selection().ordered().size() == 1);
        const auto clone = std::get<NodeId>(session.selection().ordered()[0]);
        const int cloneId = *vmfId(doc, clone);
        record(doc, "duplicate", clone);
        session.selection().select(deleted);
        record(doc, "delete", deleted);
        REQUIRE(session.deleteSelection());
        REQUIRE_FALSE(doc.handle(deleted));

        session.selection().select(solid);
        session.selection().add(entity);
        REQUIRE(session.groupSelection());
        const auto group = session.groups().parentGroup(solid);
        REQUIRE(group);
        REQUIRE(session.groups().parentGroup(entity) == group);
        const int groupId = *vmfId(doc, *group);
        record(doc, "created group", *group);
        session.selection().select(solid);
        REQUIRE(session.hideSelection());
        REQUIRE(session.nodeHidden(solid));
        // D11 permits wrapper reorder. Exercise unhide and hide, then match by VMF id.
        REQUIRE(session.setNodeHidden({solid}, false));
        REQUIRE_FALSE(session.nodeHidden(solid));
        REQUIRE(session.setNodeHidden({solid}, true));
        REQUIRE(session.nodeHidden(solid));
        doc.unobserve(observer);
        std::cout << audit.str();

        const auto output = doc.write();
        REQUIRE(output);
        const auto reparsed = vmf::parse(*output, {original.kvDocument().escapeMode()});
        REQUIRE(reparsed);
        Document roundtrip(*reparsed);
        CHECK(roundtrip.write().value() == *output);
        for (const auto h : nodes(roundtrip)) {
            if (std::holds_alternative<Handle<Solid>>(h) && vmfId(roundtrip, roundtrip.get(h)->id) == solidId) {
                CHECK(isHiddenByWrapper(roundtrip, roundtrip.get(h)->id));
                const auto* wrapper = roundtrip.get(*roundtrip.handle(roundtrip.get(h)->parent));
                CHECK(wrapper->block->key() == "hidden");
                CHECK(wrapper->children.size() == 1);
                CHECK(wrapper->block->children().size() == 1);
            }
        }
        const auto& a = original.kvDocument();
        const auto& b = reparsed->kvDocument();
        const auto before = blocks(a), after = blocks(b);
        const auto deletedIds = [&] {
            std::set<Identity> result{{"solid", deletedId}};
            for (const auto* side : deletedBlock.findAll("side")) result.emplace("side", side->find("id")->asInt().value());
            return result;
        }();
        std::set<Identity> created{{"solid", cloneId}, {"group", groupId}};
        const auto* cloneBlock = after.ids.at({"solid", cloneId});
        for (const auto* side : cloneBlock->findAll("side")) created.emplace("side", side->find("id")->asInt().value());
        std::set<int> transformedSides;
        for (const auto& side : beforeSolid.sides) transformedSides.insert(static_cast<int>(side.sideId));
        for (const auto& [identity, old] : before.ids) {
            CAPTURE(identity.first);
            CAPTURE(identity.second);
            if (deletedIds.contains(identity)) {
                CHECK_FALSE(after.ids.contains(identity));
                continue;
            }
            REQUIRE(after.ids.contains(identity));
            const auto* now = after.ids.at(identity);
            auto expectedParents = before.parents.at(identity);
            if (identity == Identity{"solid", solidId}) expectedParents.emplace_back("hidden");
            if (identity.first == "side" && transformedSides.contains(identity.second))
                expectedParents.insert(expectedParents.end() - 1, "hidden");
            CHECK(after.parents.at(identity) == expectedParents);
            Allowed allowed;
            if (identity == Identity{"solid", solidId})
                allowed = {{"side", "plane"}, {"side", "point_data"}, {"editor", "groupid"}};
            else if (identity == Identity{"entity", entityId})
                allowed = {{"origin"}, {"angles"}, {"targetname"}, {"editor", "groupid"}};
            else if (identity.first == "side" && transformedSides.contains(identity.second))
                allowed = {{"plane"}, {"point_data"}};
            else if (identity.first == "world") allowed = {{"solid"}, {"group"}, {"hidden"}};
            if (allowed.empty()) CHECK(text(a, *old) == text(b, *now));
            else {
                CHECK(protectedText(a, *old, allowed) == protectedText(b, *now, allowed));
                CHECK(text(a, *old) != text(b, *now));
            }
        }
        for (const auto& [identity, unused] : after.ids) {
            (void)unused;
            CHECK((before.ids.contains(identity) || created.contains(identity)));
        }
        CHECK(after.ids.size() == before.ids.size() - deletedIds.size() + created.size());
        // Top-level opaque metadata and all original hidden subtrees also survive verbatim.
        CHECK(protectedText(a, a.root(), {{"world"}, {"entity"}, {"hidden"}}) ==
              protectedText(b, b.root(), {{"world"}, {"entity"}, {"hidden"}}));
        for (const auto& wrapper : before.hidden) CHECK(after.hidden.count(wrapper) >= before.hidden.count(wrapper));

        const auto* finalSolid = after.ids.at({"solid", solidId});
        const auto finalGeometry = geometry::solidGeometry(*finalSolid);
        REQUIRE(finalGeometry);
        CHECK(finalGeometry->poly.validate().empty());
        CHECK_FALSE(finalGeometry->usedPlaneFallback);
        REQUIRE(finalGeometry->poly.vertices.size() == beforeSolid.poly.vertices.size());
        for (const auto p : beforeSolid.poly.vertices) {
            CHECK(std::ranges::any_of(finalGeometry->poly.vertices, [&](DVec3 q) {
                return glm::length(q - rearranged(p)) < 1e-6;
            }));
        }
        // Check saved plane strings as well as point_data: neither representation may
        // silently disagree with the intended transform while the other masks it.
        for (const auto& oldFace : beforeSolid.poly.faces) {
            const auto* side = after.ids.at({"side", static_cast<int>(oldFace.sideId)});
            auto planeValue = *side->find("plane");
            const auto points = vmf::Value(&planeValue).asPlane();
            REQUIRE(points);
            const auto p = [](const vmf::Vec3& v) { return DVec3{v[0], v[1], v[2]}; };
            const auto plane = geometry::planePointsFromSource(p(points->points[0]), p(points->points[1]), p(points->points[2]));
            REQUIRE(plane);
            checkPoint(plane->normal, {-oldFace.plane.normal.y, oldFace.plane.normal.x, oldFace.plane.normal.z});
            for (const auto vertex : oldFace.vertices)
                CHECK(std::abs(plane->signedDistance(rearranged(beforeSolid.poly.vertices[vertex]))) < 1e-6);
        }
        const auto* finalEntity = after.ids.at({"entity", entityId});
        const auto finalOrigin = finalEntity->find("origin")->asVec3().value();
        checkPoint({finalOrigin[0], finalOrigin[1], finalOrigin[2]}, rearranged({origin[0], origin[1], origin[2]}));
        const auto finalAngles = finalEntity->find("angles")->asVec3().value();
        for (int i = 0; i < 3; ++i)
            CHECK(std::abs(std::remainder(finalAngles[i] - angles[i] - (i == 1 ? 90.0 : 0.0), 360.0)) < 1e-6);
        CHECK(finalEntity->find("targetname")->decodedValue() == newName);
        CHECK(finalSolid->find("editor")->find("groupid")->asInt() == groupId);
        CHECK(finalEntity->find("editor")->find("groupid")->asInt() == groupId);
        CHECK(cloneFields(*finalSolid) == cloneFields(*cloneBlock));
        const auto* groupBlock = after.ids.at({"group", groupId});
        REQUIRE(groupBlock->children().size() == 2);
        const auto* editor = groupBlock->find("editor");
        REQUIRE(editor);
        CHECK(editor->children().size() == (parentGroupId ? 2 : 1));
        CHECK(editor->find("color")->decodedValue() == groupColor(groupId));
        if (parentGroupId) CHECK(editor->find("groupid")->asInt() == *parentGroupId);
        if (const char* out = std::getenv("APEX_REARRANGE_OUT"); out && *out) {
            const auto root = std::filesystem::path(std::getenv("APEX_P2CE_DIR")) / "sdk_content/maps";
            const auto destination = std::filesystem::path(out) / path.lexically_relative(root);
            std::filesystem::create_directories(destination.parent_path());
            std::ofstream stream(destination, std::ios::binary);
            REQUIRE(stream.good());
            stream << *output;
            stream.close();
            REQUIRE(stream.good());
            std::ofstream manifest(destination.string() + ".edits.txt", std::ios::binary);
            manifest << audit.str();
            manifest.close();
            REQUIRE(manifest.good());
        }
    }
}
