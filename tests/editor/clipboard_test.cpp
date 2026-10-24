#include "fixtures.hpp"
#include "editor/editor_session.hpp"
#include "core/strings.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>

using namespace editor_test;

namespace {

using Kv = kv1::Node;

std::size_t count(const std::string& text, std::string_view needle) {
    std::size_t n = 0;
    for (auto at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++n;
    return n;
}
std::string bytes(const Document& doc) {
    auto text = doc.write();
    REQUIRE(text);
    return *text;
}
bool isSolid(const Document& doc, NodeId n) { return std::holds_alternative<Handle<Solid>>(*doc.handle(n)); }
bool isGroupNode(const Document& doc, NodeId n) { return std::holds_alternative<Handle<Group>>(*doc.handle(n)); }

std::string valueOf(const Kv* parent, std::string_view key) {
    const auto* node = parent ? parent->find(key) : nullptr;
    if (!node) return {};
    auto v = node->decodedValue();
    return v ? *v : std::string();
}
int intOf(const Kv* parent, std::string_view key) {
    const auto* node = parent ? parent->find(key) : nullptr;
    const auto v = node ? node->asInt() : Result<int>(std::unexpected(Error{ErrorCode::NotFound, ""}));
    return v ? *v : -1;
}

struct Map {
    Document doc{parsed("versioninfo\n{\n\"editorversion\" \"400\"\n}\nworld\n{\n\"id\" \"1\"\n\"classname\" \"worldspawn\"\n}\n")};
    EditorSession session;
    NodeHandle world;

    Map() {
        for (const auto r : doc.roots()) {
            if (doc.get(*doc.handle(r))->block->key() == "world") world = *doc.handle(r);
        }
        session.attach(doc);
        session.perform("selection.mode.objects");
    }
    NodeHandle box(DVec3 min, DVec3 max) { return addBox(doc, min, max, world); }
    NodeHandle group() { return addBlock(doc, "group\n{\n\"id\" \"0\"\n}", world); }
    void edit(NodeHandle h, EditorEdit e) {
        auto tx = doc.begin("edit");
        tx.setEditor(h, std::move(e));
        tx.commit();
    }
    int vid(NodeHandle h) { return *vmfId(doc, id(doc, h)); }
    NodeHandle brushEntity(std::string classname, std::vector<std::pair<DVec3, DVec3>> boxes, std::string name = "") {
        auto entity = Kv::block("entity");
        entity.append(Kv::value("id", "0")).value();
        entity.append(Kv::value("classname", classname)).value();
        if (!name.empty()) entity.append(Kv::value("targetname", name)).value();
        for (const auto& [lo, hi] : boxes)
            entity.append(solidBlock(geometry_test::build(geometry_test::boxSides(lo, hi)))).value();
        auto tx = doc.begin("entity");
        auto h = tx.create(std::move(entity));
        tx.commit();
        return h;
    }
    NodeHandle point(std::string classname, std::string name, DVec3 origin, std::string extra = "") {
        std::ostringstream text;
        text << "entity\n{\n\"id\" \"0\"\n\"classname\" \"" << classname << "\"\n";
        if (!name.empty()) text << "\"targetname\" \"" << name << "\"\n";
        text << "\"origin\" \"" << origin.x << ' ' << origin.y << ' ' << origin.z << "\"\n" << extra << "}";
        return addBlock(doc, text.str());
    }
    void selectIds(std::vector<NodeId> ids) {
        session.selection().clear();
        for (const auto n : ids) session.selection().add(n);
    }
    void mode(const char* action) { session.perform(action); }
    void select(std::vector<NodeHandle> hs) {
        session.selection().clear();
        for (const auto h : hs) session.selection().add(id(doc, h));
    }
    std::size_t groupCount() const {
        std::size_t n = 0;
        for (const auto h : nodes(doc)) n += std::holds_alternative<Handle<Group>>(h);
        return n;
    }
    std::vector<NodeId> worldSolids() const {
        std::vector<NodeId> out;
        for (const auto c : doc.get(world)->children)
            if (isSolid(doc, c)) out.push_back(c);
        return out;
    }
    std::vector<NodeId> solidsIn(NodeId entity) const {
        std::vector<NodeId> out;
        for (const auto c : doc.get(*doc.handle(entity))->children)
            if (isSolid(doc, c)) out.push_back(c);
        return out;
    }
    std::vector<NodeId> entities() const {
        std::vector<NodeId> out;
        for (const auto r : doc.roots()) {
            const auto* n = doc.get(*doc.handle(r));
            if (std::holds_alternative<Handle<Entity>>(*doc.handle(r)) && n->block && n->block->key() == "entity")
                out.push_back(r);
        }
        return out;
    }
    std::vector<NodeId> selected() {
        std::vector<NodeId> out;
        for (const auto& t : session.selection().ordered()) out.push_back(std::get<NodeId>(t));
        return out;
    }
};

AABB boundsOf(const Document& doc, NodeId solid) {
    AABB b;
    const auto g = geometry::solidGeometry(*doc.get(*doc.handle(solid))->block);
    REQUIRE(g);
    for (const auto v : g->poly.vertices) b.expand(v);
    return b;
}

// Object ids (entities, solids), group ids and side ids are each unique across the document
// (the document allocates groups from their own counter).
void checkUniqueIds(const Document& doc) {
    std::set<std::string> objects, groups, sides;
    std::size_t objectCount = 0, groupCount = 0, sideCount = 0;
    for (const auto h : nodes(doc)) {
        const auto* node = doc.get(h);
        if (!node->block) continue;
        if (std::holds_alternative<Handle<Group>>(h)) {
            ++groupCount;
            groups.insert(valueOf(node->block.get(), "id"));
            continue;
        }
        if (!std::holds_alternative<Handle<Solid>>(h) && !std::holds_alternative<Handle<Entity>>(h)) continue;
        if (node->block->key() == "world") continue; // The world's id is not part of the allocator.
        ++objectCount;
        objects.insert(valueOf(node->block.get(), "id"));
        if (std::holds_alternative<Handle<Solid>>(h)) {
            for (const auto* side : node->block->findAll("side")) {
                ++sideCount;
                sides.insert(valueOf(side, "id"));
            }
        }
    }
    CHECK(objects.size() == objectCount);
    CHECK(groups.size() == groupCount);
    CHECK(sides.size() == sideCount);
}

// Every editor groupid names an existing group.
void checkGroupRefs(const Document& doc) {
    std::set<int> groups;
    for (const auto h : nodes(doc)) {
        if (std::holds_alternative<Handle<Group>>(h)) groups.insert(*vmfId(doc, doc.get(h)->id));
    }
    for (const auto h : nodes(doc)) {
        const auto* node = doc.get(h);
        if (!node->block) continue;
        const auto* editor = node->block->find("editor");
        if (!editor || !editor->find("groupid")) continue;
        CHECK(groups.contains(intOf(editor, "groupid")));
    }
}

int visgroupId(const Document& doc, std::string_view name) {
    for (const auto h : nodes(doc)) {
        if (!std::holds_alternative<Handle<Visgroup>>(h)) continue;
        const auto* node = doc.get(h);
        if (valueOf(node->block.get(), "name") == name) return intOf(node->block.get(), "visgroupid");
    }
    return -1;
}
std::vector<int> visgroupsOfNode(const Document& doc, NodeId n) {
    std::vector<int> out;
    const auto* node = doc.get(*doc.handle(n));
    const Kv* editor = node->block->find("editor");
    if (!editor) { // An entity's editor block is a child node.
        for (const auto c : node->children) {
            const auto* cn = doc.get(*doc.handle(c));
            if (cn->block && cn->block->key() == "editor") editor = cn->block.get();
        }
    }
    if (editor) {
        for (const auto* v : editor->findAll("visgroupid")) out.push_back(*v->asInt());
    }
    return out;
}
std::string targetname(const Document& doc, NodeId n) { return valueOf(doc.get(*doc.handle(n))->block.get(), "targetname"); }

// Group {gbox, relay}, brush entity "door" (two solids) with connections-free body, loose box.
struct Source {
    Map m;
    NodeHandle g, gbox, relay, door, loose;
    int visId = 0, subId = 0;
    Source() {
        addBlock(m.doc,
                 "visgroups\n{\n visgroup\n {\n \"name\" \"Vis A\"\n \"visgroupid\" \"0\"\n \"color\" \"10 20 30\"\n"
                 "  visgroup\n  {\n  \"name\" \"Sub\"\n  \"visgroupid\" \"0\"\n  }\n }\n}");
        visId = visgroupId(m.doc, "Vis A");
        subId = visgroupId(m.doc, "Sub");
        g = m.group();
        gbox = m.box({0, 0, 0}, {64, 64, 64});
        m.edit(gbox, EditorEdit::setGroup(m.vid(g)));
        m.edit(gbox, EditorEdit::addVisgroup(visId));
        relay = m.point("logic_relay", "relay", {128, 0, 0},
                        "connections\n{\n\"OnTrigger\" \"door,Open,,0,-1\"\n\"OnSpawn\" \"relay2,Trigger,,0.5,1\"\n}\n");
        m.edit(relay, EditorEdit::setGroup(m.vid(g)));
        door = m.brushEntity("func_door", {{{256, 0, 0}, {320, 64, 64}}, {{256, 128, 0}, {320, 192, 64}}}, "door");
        m.edit(door, EditorEdit::addVisgroup(subId));
        loose = m.box({512, 0, 0}, {576, 64, 64});
    }
    NodeId n(NodeHandle h) { return id(m.doc, h); }
};

} // namespace

TEST_CASE("Paste into the same map keeps connections, remaps groups, and is one undo step") {
    Source s;
    auto& m = s.m;
    m.mode("selection.mode.groups");
    m.select({s.g, s.door});
    const auto before = bytes(m.doc);
    const auto undo = m.doc.undoCount();
    const auto text = m.session.copyText();
    REQUIRE(text);
    CHECK(count(*text, "connections") == 1);
    CHECK(count(*text, "visgroup") >= 2);

    REQUIRE(m.session.pasteText(*text));
    CHECK(m.doc.undoCount() == undo + 1);
    const auto after = bytes(m.doc);
    CHECK(count(after, "door,Open,,0,-1") == 2);
    CHECK(count(after, "relay2,Trigger,,0.5,1") == 2);
    CHECK(count(after, "\"Vis A\"") == 1); // Matched by name, not duplicated.
    CHECK(m.groupCount() == 2);
    checkUniqueIds(m.doc);
    checkGroupRefs(m.doc);
    // The pasted group is selected, and is a different group from the original.
    REQUIRE(m.selected().size() == 2);
    CHECK_FALSE(m.session.selection().contains(s.n(s.g)));
    CHECK_FALSE(m.session.selection().contains(s.n(s.door)));
    std::size_t groupsSelected = 0;
    for (const auto id : m.selected()) {
        if (isGroupNode(m.doc, id)) {
            ++groupsSelected;
            CHECK(m.session.groups().members(id).size() == 2);
        }
    }
    CHECK(groupsSelected == 1);
    // Group members of the original group are untouched.
    CHECK(m.session.groups().members(s.n(s.g)).size() == 2);

    REQUIRE(m.doc.undo());
    CHECK(bytes(m.doc) == before);
}

TEST_CASE("Paste into another map maps visgroups by name and creates the missing ones") {
    Source s;
    auto& m = s.m;
    m.select({s.gbox, s.door});
    const auto text = m.session.copyText();
    REQUIRE(text);

    Map t;
    addBlock(t.doc, "visgroups\n{\n visgroup\n {\n \"name\" \"Other\"\n \"visgroupid\" \"0\"\n }\n"
                    " visgroup\n {\n \"name\" \"vis a\"\n \"visgroupid\" \"0\"\n }\n}");
    const int targetVis = visgroupId(t.doc, "vis a");
    REQUIRE(targetVis > 0);
    REQUIRE(t.session.pasteText(*text));
    checkUniqueIds(t.doc);

    // "Vis A" matched the existing "vis a" case-insensitively; "Sub" was created beneath it.
    CHECK(visgroupId(t.doc, "Vis A") == -1);
    const int sub = visgroupId(t.doc, "Sub");
    REQUIRE(sub > 0);
    for (const auto h : nodes(t.doc)) {
        if (!std::holds_alternative<Handle<Visgroup>>(h)) continue;
        const auto* node = t.doc.get(h);
        if (valueOf(node->block.get(), "name") != "Sub") continue;
        CHECK(valueOf(t.doc.get(*t.doc.handle(node->parent))->block.get(), "name") == "vis a");
    }
    // The pasted objects carry the target's visgroup ids.
    REQUIRE(t.selected().size() == 2);
    std::set<int> ids;
    for (const auto n : t.selected())
        for (const auto v : visgroupsOfNode(t.doc, n)) ids.insert(v);
    CHECK(ids == std::set<int>{targetVis, sub});

    // A map with no visgroups block gets one, placed after versioninfo.
    Map u;
    REQUIRE(u.session.pasteText(*text));
    CHECK(visgroupId(u.doc, "Vis A") > 0);
    CHECK(visgroupId(u.doc, "Sub") > 0);
    CHECK(kv1::parse(bytes(u.doc)).has_value());
}

TEST_CASE("Paste places the bounds on the work plane, snapped, or in place") {
    Map m;
    const auto a = m.box({5, 5, 5}, {69, 69, 69});
    m.select({a});
    const auto text = m.session.copyText();
    REQUIRE(text);

    PasteOptions options;
    options.at = DVec3{256, 128, 0};
    REQUIRE(m.session.pasteText(*text, options));
    REQUIRE(m.selected().size() == 1);
    const auto placed = boundsOf(m.doc, m.selected()[0]);
    CHECK(placed.min.z == doctest::Approx(0.0));
    const auto centre = placed.center();
    CHECK(std::abs(centre.x - 256) <= m.session.gridSize());
    CHECK(std::abs(centre.y - 128) <= m.session.gridSize());
    // Grid-aligned in the plane (absolute snapping).
    CHECK(std::fmod(std::abs(placed.min.x), m.session.gridSize()) == doctest::Approx(0.0));

    PasteOptions inPlace;
    inPlace.inPlace = true;
    REQUIRE(m.session.pasteText(*text, inPlace));
    const auto same = boundsOf(m.doc, m.selected()[0]);
    CHECK(same.min == DVec3{5, 5, 5});
    CHECK(same.max == DVec3{69, 69, 69});
    checkUniqueIds(m.doc);

    // A paste onto a different plane puts the box on it.
    PasteOptions tilted;
    tilted.plane = SurfacePlane{{0, 0, 128}, {0, 0, 1}, {1, 0, 0}, {0, 1, 0}, {}};
    REQUIRE(m.session.pasteText(*text, tilted));
    CHECK(boundsOf(m.doc, m.selected()[0]).min.z == doctest::Approx(128.0));
}

TEST_CASE("Paste special makes unique entity names, accumulates offset and groups copies") {
    Map m;
    const auto door = m.brushEntity("func_door", {{{0, 0, 0}, {64, 64, 64}}}, "door");
    const auto relay = m.point("logic_relay", "relay", {0, 200, 0}, "connections\n{\n\"OnTrigger\" \"door,Open,,0,-1\"\n}\n");
    m.point("info_target", "c_1_door", {900, 0, 0}); // Already taken: the first copy must not reuse it.
    m.select({door, relay});
    const auto text = m.session.copyText();
    REQUIRE(text);

    PasteSpecial special;
    special.copies = 3;
    special.offset = {0, 128, 0};
    special.names = NamePolicy::Prefix;
    special.affix = "c_";
    special.rewriteReferences = true;
    special.groupCopies = true;
    PasteOptions options;
    options.inPlace = true;
    options.special = special;
    const auto before = bytes(m.doc);
    REQUIRE(m.session.pasteText(*text, options));

    std::vector<std::string> names;
    for (const auto e : m.entities()) names.push_back(targetname(m.doc, e));
    std::set<std::string> unique(names.begin(), names.end());
    CHECK(unique.size() == names.size());
    CHECK(unique.contains("c_1_door_2"));
    CHECK(unique.contains("c_2_door"));
    CHECK(unique.contains("c_3_relay"));
    // References follow the rename inside each copy only.
    CHECK(count(bytes(m.doc), "\"door,Open,,0,-1\"") == 1);
    CHECK(count(bytes(m.doc), "\"c_2_door,Open,,0,-1\"") == 1);
    CHECK(count(bytes(m.doc), "\"c_1_door_2,Open,,0,-1\"") == 1);
    // Three copies, each one group of two members; Groups-mode selection is not in play here
    // (Objects mode), so the six pasted objects are selected.
    CHECK(m.groupCount() == 3);
    CHECK(m.selected().size() == 6);
    // Offsets accumulate: the brush of copy n sits n * 128 up in Y.
    std::set<double> ys;
    for (const auto n : m.selected()) {
        if (isSolid(m.doc, n)) continue;
        const auto solids = m.solidsIn(n);
        if (!solids.empty()) ys.insert(boundsOf(m.doc, solids[0]).min.y);
    }
    CHECK(ys == std::set<double>{128, 256, 384});
    checkUniqueIds(m.doc);
    checkGroupRefs(m.doc);
    REQUIRE(m.doc.undo());
    CHECK(bytes(m.doc) == before);

    // Without rewriteReferences the connection stays verbatim in every copy; suffix + %d.
    options.special->rewriteReferences = false;
    options.special->groupCopies = false;
    options.special->names = NamePolicy::Suffix;
    options.special->affix = "_x%d";
    REQUIRE(m.session.pasteText(*text, options));
    CHECK(count(bytes(m.doc), "\"door,Open,,0,-1\"") == 4);
    CHECK(count(bytes(m.doc), "\"door_x2\"") == 1);
    CHECK(m.groupCount() == 0);
}

TEST_CASE("Paste special rotates each copy about its own bounds centre") {
    Map m;
    const auto a = m.point("info_target", "a", {0, 0, 0});
    const auto b = m.point("info_target", "b", {128, 0, 0});
    m.select({a, b});
    const auto text = m.session.copyText();
    REQUIRE(text);
    PasteSpecial special;
    special.rotationDeg = {0, 0, 90};
    PasteOptions options;
    options.inPlace = true;
    options.special = special;
    REQUIRE(m.session.pasteText(*text, options));
    REQUIRE(m.selected().size() == 2);
    std::vector<DVec3> origins;
    for (const auto n : m.selected()) {
        const auto* block = m.doc.get(*m.doc.handle(n))->block.get();
        const auto o = block->find("origin")->asVec3();
        REQUIRE(o);
        origins.push_back({(*o)[0], (*o)[1], (*o)[2]});
        CHECK(valueOf(block, "angles") == "0 90 0");
    }
    checkPoint(origins[0], {64, -64, 0});
    checkPoint(origins[1], {64, 64, 0});

    PasteSpecial bad;
    bad.copies = 0;
    options.special = bad;
    const auto before = bytes(m.doc);
    CHECK_FALSE(m.session.pasteText(*text, options));
    bad.copies = 1000;
    options.special = bad;
    CHECK_FALSE(m.session.pasteText(*text, options));
    CHECK(bytes(m.doc) == before);
}

TEST_CASE("Duplicate pastes in place without a clipboard and bad input leaves the document alone") {
    Source s;
    auto& m = s.m;
    m.select({s.loose, s.door});
    const auto before = bytes(m.doc);
    const auto undo = m.doc.undoCount();
    REQUIRE(m.session.duplicateSelection());
    CHECK(m.doc.undoCount() == undo + 1);
    CHECK(m.worldSolids().size() == 3); // gbox, loose and the copy of loose; the door copy's solids sit in its entity.
    REQUIRE(m.selected().size() == 2);
    CHECK_FALSE(m.session.selection().contains(s.n(s.loose)));
    const auto copy = m.selected();
    checkUniqueIds(m.doc);
    REQUIRE(m.doc.undo());
    CHECK(bytes(m.doc) == before);

    CHECK_FALSE(m.session.pasteText("not a fragment"));
    CHECK_FALSE(m.session.pasteText("visgroups\n{\n}\n"));
    CHECK(bytes(m.doc) == before);
    m.session.selection().clear();
    CHECK_FALSE(m.session.copyText());
    CHECK_FALSE(m.session.duplicateSelection());
    m.mode("selection.mode.faces");
    CHECK_FALSE(m.session.copyText());
    CHECK(bytes(m.doc) == before);
    (void)copy;
}

TEST_CASE("Delete handles brush entity solids, recursive groups and empty containers") {
    Source s;
    auto& m = s.m;
    const NodeId doorId = s.n(s.door), looseId = s.n(s.loose);
    const auto doorSolids = m.solidsIn(doorId);
    REQUIRE(doorSolids.size() == 2);
    const auto before = bytes(m.doc);

    // One of two brush entity solids: the entity stays.
    m.mode("selection.mode.solids");
    m.session.selection().add(doorSolids[0]);
    REQUIRE(m.session.deleteSelection());
    CHECK(m.doc.handle(doorId).has_value());
    CHECK(m.solidsIn(doorId).size() == 1);
    CHECK(m.selected().empty());
    REQUIRE(m.doc.undo());
    CHECK(bytes(m.doc) == before);

    // All of them: the empty brush entity goes too.
    m.session.selection().clear();
    m.session.selection().add(doorSolids[0]);
    m.session.selection().add(doorSolids[1]);
    REQUIRE(m.session.deleteSelection());
    CHECK_FALSE(m.doc.handle(doorId).has_value());
    REQUIRE(m.doc.undo());
    CHECK(bytes(m.doc) == before);

    // A group deletes with all members, nested groups included.
    const auto inner = m.group();
    const auto innerBox = m.box({1000, 0, 0}, {1064, 64, 64});
    m.edit(innerBox, EditorEdit::setGroup(m.vid(inner)));
    m.edit(inner, EditorEdit::setGroup(m.vid(s.g)));
    const auto nested = bytes(m.doc);
    const std::vector<NodeId> gone{s.n(s.g), s.n(s.gbox), s.n(s.relay), id(m.doc, inner), id(m.doc, innerBox)};
    m.mode("selection.mode.groups");
    m.select({s.g});
    REQUIRE(m.session.deleteSelection());
    for (const auto n : gone) CHECK_FALSE(m.doc.handle(n).has_value());
    CHECK(m.doc.handle(looseId).has_value());
    CHECK(m.groupCount() == 0);
    REQUIRE(m.doc.undo());
    CHECK(bytes(m.doc) == nested);

    // Deleting every member empties the group, which is deleted with them.
    m.mode("selection.mode.objects");
    m.session.selection().clear();
    for (const auto n : {gone[1], gone[2], gone[4]}) m.session.selection().add(n);
    REQUIRE(m.session.deleteSelection());
    CHECK(m.groupCount() == 0);
    CHECK_FALSE(m.session.deleteSelection()); // Nothing selected now.
    REQUIRE(m.doc.undo());
    CHECK(bytes(m.doc) == nested);
}

TEST_CASE("Cut removes the selection and undo restores the exact bytes") {
    Source s;
    auto& m = s.m;
    m.mode("selection.mode.groups");
    m.select({s.g, s.door, s.loose});
    const std::vector<NodeId> picked = m.selected();
    const auto before = bytes(m.doc);
    const auto undo = m.doc.undoCount();
    const auto text = m.session.cutText();
    REQUIRE(text);
    CHECK(m.doc.undoCount() == undo + 1);
    CHECK(m.groupCount() == 0);
    CHECK(m.worldSolids().empty());
    CHECK(m.entities().empty());
    CHECK(m.selected().empty());
    REQUIRE(m.doc.undo());
    CHECK(bytes(m.doc) == before);
    REQUIRE(m.doc.redo());
    CHECK(m.entities().empty());
    REQUIRE(m.doc.undo());

    // The cut text pastes back everything that was removed.
    PasteOptions inPlace;
    inPlace.inPlace = true;
    m.session.selection().clear();
    for (const auto n : picked) m.session.selection().add(n);
    REQUIRE(m.session.cutText());
    REQUIRE(m.session.pasteText(*text, inPlace));
    checkUniqueIds(m.doc);
    CHECK(count(bytes(m.doc), "door,Open,,0,-1") == 1);
    CHECK(m.groupCount() == 1);
}

TEST_CASE("Group and ungroup are one level, one transaction, with GroupIndex in sync") {
    Source s;
    auto& m = s.m;
    const auto before = bytes(m.doc);
    const NodeId gId = s.n(s.g), gboxId = s.n(s.gbox), relayId = s.n(s.relay);

    // Objects mode keeps the selection. Without ignore-groups an object's whole outermost
    // group joins the new group; with it, the object itself moves in.
    m.select({s.gbox, s.loose});
    const auto undo = m.doc.undoCount();
    REQUIRE(m.session.groupSelection());
    CHECK(m.doc.undoCount() == undo + 1);
    CHECK(m.groupCount() == 2);
    const auto g2 = m.session.groups().parentGroup(s.n(s.loose));
    REQUIRE(g2);
    CHECK(m.session.groups().parentGroup(s.n(s.g)) == g2);
    CHECK(m.session.groups().parentGroup(s.n(s.gbox)) == m.session.groups().group(m.vid(s.g)));
    CHECK(m.selected().size() == 2);
    REQUIRE(m.doc.undo());
    CHECK(bytes(m.doc) == before);

    m.session.selection().setIgnoreGroups(true);
    m.select({s.gbox, s.loose});
    REQUIRE(m.session.groupSelection());
    const auto g3 = m.session.groups().parentGroup(s.n(s.loose));
    REQUIRE(g3);
    CHECK(m.session.groups().parentGroup(s.n(s.gbox)) == g3);
    CHECK(m.session.groups().parentGroup(s.n(s.relay)) == m.session.groups().group(m.vid(s.g)));
    REQUIRE(m.doc.undo());
    CHECK(bytes(m.doc) == before);
    m.session.selection().setIgnoreGroups(false);

    // Groups mode: grouping a group nests it, and the new group is selected.
    m.mode("selection.mode.groups");
    m.select({s.g, s.loose});
    REQUIRE(m.session.groupSelection());
    CHECK(m.groupCount() == 2);
    const auto outer = m.session.groups().parentGroup(s.n(s.g));
    REQUIRE(outer);
    CHECK(m.session.groups().parentGroup(s.n(s.loose)) == outer);
    CHECK(m.session.groups().outermostGroup(s.n(s.gbox)) == outer);
    REQUIRE(m.selected().size() == 1);
    CHECK(m.selected()[0] == *outer);

    // Ungroup dissolves exactly one level: the old group survives with its members.
    REQUIRE(m.session.ungroupSelection());
    CHECK(m.groupCount() == 1);
    CHECK_FALSE(m.session.groups().parentGroup(s.n(s.loose)).has_value());
    CHECK(m.session.groups().parentGroup(s.n(s.gbox)) == m.session.groups().group(m.vid(s.g)));
    REQUIRE(m.selected().size() == 2);
    CHECK(m.session.selection().contains(s.n(s.g)));
    CHECK(m.session.selection().contains(s.n(s.loose)));
    REQUIRE(m.session.ungroupSelection()); // Dissolves g itself (selected).
    CHECK(m.groupCount() == 0);
    CHECK_FALSE(m.session.groups().parentGroup(s.n(s.gbox)).has_value());
    CHECK(m.session.ungroupSelection().has_value() == false); // Nothing left to ungroup.

    // Ungroup from the registry action too.
    REQUIRE(m.doc.undo());
    REQUIRE(m.doc.undo());
    REQUIRE(m.doc.undo());
    CHECK(bytes(m.doc) == before);
    m.mode("selection.mode.groups");
    m.selectIds({gId});
    CHECK(m.session.perform("edit.ungroup"));
    CHECK(m.groupCount() == 0);
    m.mode("selection.mode.objects");
    m.selectIds({gboxId, relayId});
    CHECK(m.session.perform("edit.group"));
    CHECK(m.groupCount() == 1);
    checkGroupRefs(m.doc);
}

TEST_CASE("Tie to entity and move to world round-trip the solids") {
    Source s;
    auto& m = s.m;
    const auto extra = m.box({700, 0, 0}, {764, 64, 64});
    const auto worldBefore = m.worldSolids();
    std::map<NodeId, AABB> bounds;
    for (const auto n : worldBefore) bounds[n] = boundsOf(m.doc, n);

    m.mode("selection.mode.solids");
    m.select({s.loose, extra});
    const auto undo = m.doc.undoCount();
    TieParams tie; // func_detail by default
    REQUIRE(m.session.tieToEntity(tie));
    CHECK(m.doc.undoCount() == undo + 1);
    CHECK(m.worldSolids().size() == worldBefore.size() - 2);
    const auto created = m.session.groups().document().roots().back();
    const auto* entity = m.doc.get(*m.doc.handle(created));
    CHECK(valueOf(entity->block.get(), "classname") == "func_detail");
    CHECK(m.solidsIn(created).size() == 2);
    CHECK(m.selected().size() == 2); // Solids mode: the tied solids stay selected.

    // Back to the world: same solids, same geometry, no entity left.
    REQUIRE(m.session.moveToWorld());
    CHECK_FALSE(m.doc.handle(created).has_value());
    auto after = m.worldSolids();
    std::ranges::sort(after);
    auto expected = worldBefore;
    std::ranges::sort(expected);
    CHECK(after == expected);
    for (const auto n : after) {
        const auto b = boundsOf(m.doc, n);
        CHECK(b.min == bounds[n].min);
        CHECK(b.max == bounds[n].max);
    }
    checkUniqueIds(m.doc);

    // A class with key values, then an existing entity as the target.
    m.select({s.loose});
    TieParams trigger;
    trigger.className = "trigger_multiple";
    trigger.keyValues = {{"wait", "1"}};
    REQUIRE(m.session.tieToEntity(trigger));
    CHECK(count(bytes(m.doc), "trigger_multiple") == 1);
    m.select({extra});
    TieParams into;
    into.entity = s.n(s.door);
    REQUIRE(m.session.tieToEntity(into));
    CHECK(m.solidsIn(s.n(s.door)).size() == 3);

    // An entity that is not an entity, or a missing class, fails cleanly.
    const auto snapshot = bytes(m.doc);
    m.select({s.gbox});
    TieParams badTarget;
    badTarget.entity = s.n(s.gbox);
    CHECK_FALSE(m.session.tieToEntity(badTarget));
    TieParams noClass;
    noClass.className = "  ";
    CHECK_FALSE(m.session.tieToEntity(noClass));
    CHECK(bytes(m.doc) == snapshot);
}

TEST_CASE("Tie to entity inside a group keeps the group, move to world restores membership") {
    Source s;
    auto& m = s.m;
    m.mode("selection.mode.solids");
    m.select({s.gbox});
    REQUIRE(m.session.tieToEntity());
    CHECK(m.groupCount() == 1); // The group still holds the relay and the new entity.
    const auto entity = m.session.groups().parentGroup(s.n(s.relay));
    REQUIRE(entity);
    CHECK(m.session.groups().members(*entity).size() == 2);
    CHECK(m.session.groups().parentGroup(s.n(s.gbox)) != entity); // The solid now belongs to its entity.

    REQUIRE(m.session.moveToWorld());
    CHECK(m.session.groups().parentGroup(s.n(s.gbox)) == entity);
    CHECK(m.session.groups().members(*entity).size() == 2);
    checkGroupRefs(m.doc);
}

TEST_CASE("Clipboard registry actions are bound and left to the UI layer") {
    Document doc(parsed());
    EditorSession session;
    session.attach(doc);
    for (const auto* action : {"edit.copy", "edit.cut", "edit.paste", "edit.paste_in_place", "edit.paste_special"}) {
        CAPTURE(action);
        CHECK(session.preset().bindings.contains(action));
        CHECK_FALSE(session.perform(action)); // The system clipboard is the UI layer's.
    }
    CHECK(session.perform("edit.delete") == false); // Nothing selected.
}

TEST_CASE("Corpus maps paste across documents keeping every connections block") {
    namespace fs = std::filesystem;
    const char* install = std::getenv("APEX_P2CE_DIR");
    if (!install || !*install) {
        MESSAGE("APEX_P2CE_DIR is unset; skipping clipboard corpus paste");
        return;
    }
    const fs::path root = fs::path(install) / "sdk_content/maps";
    // These two maps ship with the SDK content, not with the instances clone; skip without them.
    if (!fs::exists(root / "test1.vmf") || !fs::exists(root / "hammertest.vmf")) {
        MESSAGE("test1.vmf or hammertest.vmf is absent; skipping clipboard corpus paste");
        return;
    }
    const auto load = [&](const char* name) {
        std::ifstream stream(root / name, std::ios::binary);
        REQUIRE(stream.good());
        const std::string text{std::istreambuf_iterator<char>(stream), {}};
        auto file = vmf::parse(text);
        if (!file) file = vmf::parse(text, {kv1::EscapeMode::Legacy});
        REQUIRE(file);
        return std::move(*file);
    };
    Document source(load("test1.vmf"));
    Document target(load("hammertest.vmf"));
    EditorSession from, to;
    from.attach(source);
    to.attach(target);
    from.perform("selection.mode.groups");
    from.perform("selection.all");
    REQUIRE_FALSE(from.selection().ordered().empty());
    const auto text = from.copyText();
    REQUIRE(text);
    const auto fragmentConnections = count(*text, "connections");
    CHECK(fragmentConnections > 0);

    for (auto* destination : {&to, &from}) {
        auto& doc = *destination->document();
        const auto before = count(bytes(doc), "connections");
        REQUIRE(destination->pasteText(*text));
        CHECK(count(bytes(doc), "connections") == before + fragmentConnections);
        checkGroupRefs(doc);
        checkUniqueIds(doc);
        CHECK(doc.undo());
        CHECK(count(bytes(doc), "connections") == before);
    }
}
