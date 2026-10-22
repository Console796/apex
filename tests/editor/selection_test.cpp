#include "fixtures.hpp"
#include "editor/selection_ops.hpp"
#include "editor/selection_undo.hpp"

using namespace editor_test;

namespace {
void join(Document& doc, NodeHandle object, NodeHandle group) {
    auto tx = doc.begin("membership");
    tx.setEditor(object, EditorEdit::setGroup(doc.get(group)->block->find("id")->asInt().value()));
    tx.commit();
}
Hit hit(NodeId node, std::optional<SideId> side = {}) { return {node, side, {}, {}, 0}; }
}

TEST_CASE("Selection ordered set active observers snapshots and mode changes do not edit document") {
    Document doc(parsed());
    const auto a = id(doc, addBox(doc, DVec3{-8}, DVec3{8}));
    const auto b = id(doc, addBox(doc, DVec3{20}, DVec3{30}));
    const auto bytes = doc.write().value();
    const auto history = doc.undoCount();
    const auto dirty = doc.dirtyNodes();
    Selection sel;
    std::uint64_t notifications = 0;
    const auto token = sel.observe([&](const Selection& current) {
        ++notifications;
        CHECK(current.changed() == notifications);
    });
    sel.select(a);
    sel.add(b);
    sel.add(a);
    CHECK(sel.ordered() == std::vector<Target>{a, b});
    CHECK(sel.active() == Target{a});
    const auto count = sel.changed();
    sel.add(a);
    CHECK(sel.changed() == count);
    Selection snapshot = sel;
    snapshot.remove(a);
    CHECK(notifications == count);
    CHECK(snapshot.active() == Target{b});
    sel.toggle(a);
    CHECK(sel.active() == Target{b});
    sel.toggle(a);
    CHECK(sel.ordered() == std::vector<Target>{b, a});
    sel.remove(b);
    CHECK(sel.active() == Target{a});
    sel.clear();
    CHECK_FALSE(sel.active());
    sel.select(a);
    sel.setMode(SelectionMode::Faces);
    CHECK(sel.ordered().empty());
    CHECK_THROWS_AS(sel.select(a), std::invalid_argument);
    sel.select(SideId{1});
    sel.setIgnoreGroups(true);
    sel.setIgnoreGroups(true);
    sel.unobserve(token);
    sel.clear();
    CHECK(doc.write().value() == bytes);
    CHECK(doc.undoCount() == history);
    CHECK(doc.dirtyNodes() == dirty);
}

TEST_CASE("Selection blobs preserve all modes policy ordering active and reject malformed data atomically") {
    for (const auto mode : {SelectionMode::Groups, SelectionMode::Objects, SelectionMode::Solids, SelectionMode::Faces}) {
        Selection sel(mode);
        const Target a = mode == SelectionMode::Faces ? Target{SideId{7}} : Target{NodeId{7}};
        const Target b = mode == SelectionMode::Faces ? Target{SideId{9}} : Target{NodeId{9}};
        sel.add(a);
        sel.add(b);
        sel.add(a);
        sel.setIgnoreGroups(true);
        const auto blob = sel.toBlob();
        Selection restored;
        CHECK(restored.fromBlob(blob));
        CHECK(restored.toBlob() == blob);
        CHECK(restored.active() == a);
        CHECK(restored.ordered() == std::vector<Target>{a, b});
        auto bad = blob;
        for (std::size_t i = 0; i < 6; ++i) {
            bad = blob;
            bad[i] = std::numeric_limits<std::uint64_t>::max();
            CHECK_FALSE(restored.fromBlob(bad));
            CHECK(restored.toBlob() == blob);
        }
        bad = blob;
        bad.back() = bad[6];
        CHECK_FALSE(restored.fromBlob(bad));
        CHECK_FALSE(restored.fromBlob({}));
        sel.clear();
        CHECK(restored.fromBlob(sel.toBlob()));
        CHECK(restored.ordered().empty());
        CHECK_FALSE(restored.active());
    }
}

TEST_CASE("Selection resolves each mode nested groups brush entities and ignore groups") {
    Document doc(parsed());
    const auto world = addBlock(doc, "world { classname worldspawn }");
    const auto outer = addBlock(doc, "group {}", world);
    const auto inner = addBlock(doc, "group {}", world);
    const auto entity = addBlock(doc, "entity { classname func_detail }");
    const auto wrapper = addBlock(doc, "container {}", entity);
    const auto brush = addBox(doc, DVec3{-8}, DVec3{8}, wrapper);
    const auto point = addBlock(doc, "entity { classname info_target origin \"30 0 0\" }");
    const auto loose = addBox(doc, {40, -8, -8}, {50, 8, 8}, world);
    join(doc, entity, inner);
    join(doc, inner, outer);
    join(doc, point, outer);
    GroupIndex groups(doc);
    PickIndex picks(doc);
    const auto picked = picks.first({{-20, 0, 0}, {1, 0, 0}});
    REQUIRE(picked);
    CHECK(resolveClick(*picked, SelectionMode::Groups, groups) == Target{id(doc, outer)});
    CHECK(resolveClick(*picked, SelectionMode::Groups, groups, true) == Target{id(doc, entity)});
    CHECK(resolveClick(*picked, SelectionMode::Objects, groups) == Target{id(doc, entity)});
    CHECK(resolveClick(*picked, SelectionMode::Solids, groups) == Target{id(doc, brush)});
    CHECK(resolveClick(*picked, SelectionMode::Faces, groups) == Target{*picked->side});
    CHECK(resolveClick(hit(id(doc, point)), SelectionMode::Groups, groups) == Target{id(doc, outer)});
    CHECK(resolveClick(hit(id(doc, loose)), SelectionMode::Groups, groups) == Target{id(doc, loose)});
    CHECK_FALSE(resolveClick(hit(id(doc, point)), SelectionMode::Solids, groups));
    CHECK_FALSE(resolveClick(hit(id(doc, point)), SelectionMode::Faces, groups));
    CHECK_FALSE(resolveClick(hit(id(doc, world)), SelectionMode::Objects, groups));
    CHECK_FALSE(resolveClick(hit(999999), SelectionMode::Groups, groups));
    const Ray ray{{-20, 0, 0}, {1, 0, 0}};
    CHECK(hover(picks, groups, ray, SelectionMode::Groups) == Target{id(doc, outer)});
    CHECK(hover(picks, groups, ray, SelectionMode::Faces) == Target{*picked->side});
    CHECK_FALSE(hover(picks, groups, {{-20, 100, 0}, {1, 0, 0}}, SelectionMode::Objects));
}

TEST_CASE("Selection click modifiers misses and depth cycling reach third occluded brush and wrap") {
    Document doc(parsed());
    const auto a = id(doc, addBox(doc, {-8, -8, -8}, {8, 8, 8}));
    const auto b = id(doc, addBox(doc, {20, -8, -8}, {30, 8, 8}));
    const auto c = id(doc, addBox(doc, {40, -8, -8}, {50, 8, 8}));
    PickIndex picks(doc);
    GroupIndex groups(doc);
    Selection sel(SelectionMode::Solids);
    const Ray ray{{-20, 0, 0}, {1, 0, 0}};
    const Ray miss{{-20, 100, 0}, {1, 0, 0}};
    CycleState cycle;
    for (const auto expected : {a, b, c, a}) {
        click(sel, picks, groups, ray, {}, &cycle, true);
        CHECK(sel.ordered() == std::vector<Target>{expected});
    }
    click(sel, picks, groups, ray, {.ctrl = true});
    CHECK(sel.ordered().empty());
    click(sel, picks, groups, ray, {.shift = true});
    click(sel, picks, groups, {{10, 0, 0}, {1, 0, 0}}, {.shift = true});
    CHECK(sel.ordered() == std::vector<Target>{a, b});
    click(sel, picks, groups, miss, {.ctrl = true});
    click(sel, picks, groups, miss, {.shift = true});
    CHECK(sel.ordered() == std::vector<Target>{a, b});
    // A repeated click skips selected targets and still reaches the rear brush.
    click(sel, picks, groups, ray, {.shift = true}, &cycle, false);
    click(sel, picks, groups, ray, {.shift = true}, &cycle, true);
    CHECK(sel.active() == Target{c});
    click(sel, picks, groups, miss);
    CHECK(sel.ordered().empty());
    click(sel, picks, groups, ray, {}, &cycle, false);
    CHECK(sel.active() == Target{a});
    sel.setMode(SelectionMode::Faces);
    click(sel, picks, groups, ray, {}, &cycle, true);
    CHECK(sel.active() == Target{*picks.first(ray)->side});
}

TEST_CASE("Selection depth cycling deduplicates brush entity and nested group targets") {
    Document doc(parsed());
    const auto group = addBlock(doc, "group {}");
    const auto entity = addBlock(doc, "entity { classname func_detail }");
    addBox(doc, {-8, -8, -8}, {8, 8, 8}, entity);
    addBox(doc, {20, -8, -8}, {30, 8, 8}, entity);
    const auto rear = addBox(doc, {40, -8, -8}, {50, 8, 8});
    join(doc, entity, group);
    PickIndex picks(doc);
    GroupIndex groups(doc);
    Selection sel;
    CycleState cycle;
    const Ray ray{{-20, 0, 0}, {1, 0, 0}};
    click(sel, picks, groups, ray, {}, &cycle, false);
    CHECK(sel.active() == Target{id(doc, group)});
    REQUIRE(cycle.candidates.size() == 2);
    click(sel, picks, groups, ray, {}, &cycle, true);
    CHECK(sel.active() == Target{id(doc, rear)});
    click(sel, picks, groups, ray, {}, &cycle, true);
    CHECK(sel.active() == Target{id(doc, group)});
}

TEST_CASE("Selection marquee enclosed touching groups brush entities faces and modifiers") {
    Document doc(parsed());
    const auto outer = addBlock(doc, "group {}");
    const auto inner = addBlock(doc, "group {}");
    const auto a = addBox(doc, {-8, -8, -8}, {8, 8, 8});
    const auto b = addBox(doc, {20, -8, -8}, {30, 8, 8});
    join(doc, inner, outer);
    join(doc, a, inner);
    join(doc, b, outer);
    const auto hidden = addBlock(doc, "hidden {}");
    const auto invisible = addBox(doc, {200, -8, -8}, {210, 8, 8}, hidden);
    join(doc, invisible, outer);
    PickIndex picks(doc);
    GroupIndex groups(doc);
    Selection sel;
    const auto partial = planes({-9, -9, -9}, {22, 9, 9});
    const auto complete = planes({-9, -9, -9}, {31, 9, 9});
    marquee(sel, picks, groups, partial, MarqueeMode::Enclosed);
    CHECK(sel.ordered().empty());
    marquee(sel, picks, groups, partial, MarqueeMode::Touching);
    CHECK(sel.ordered() == std::vector<Target>{id(doc, outer)});
    marquee(sel, picks, groups, complete, MarqueeMode::Enclosed);
    CHECK(sel.ordered() == std::vector<Target>{id(doc, outer)});
    marquee(sel, picks, groups, complete, MarqueeMode::Touching, {.ctrl = true});
    CHECK(sel.ordered().empty());
    sel.setIgnoreGroups(true);
    marquee(sel, picks, groups, partial, MarqueeMode::Enclosed);
    CHECK(sel.ordered() == std::vector<Target>{id(doc, a)});
    marquee(sel, picks, groups, complete, MarqueeMode::Touching, {.shift = true});
    CHECK(sel.ordered() == std::vector<Target>{id(doc, a), id(doc, b)});
    marquee(sel, picks, groups, complete, MarqueeMode::Touching, {.ctrl = true, .shift = true});
    CHECK(sel.ordered().empty());
    sel.setMode(SelectionMode::Faces);
    marquee(sel, picks, groups, partial, MarqueeMode::Enclosed);
    CHECK(sel.ordered().size() == 6);
    for (const auto target : sel.ordered()) CHECK(doc.side(std::get<SideId>(target))->solid == std::get<Handle<Solid>>(a));
    marquee(sel, picks, groups, partial, MarqueeMode::Touching);
    CHECK(sel.ordered().size() == 12);
    sel.setMode(SelectionMode::Objects);
    const auto entity = addBlock(doc, "entity { classname func_detail }");
    auto tx = doc.begin("brush entity");
    tx.reparent(a, entity);
    tx.reparent(b, entity);
    tx.commit();
    picks.rebuild();
    groups.rebuild();
    marquee(sel, picks, groups, partial, MarqueeMode::Enclosed);
    CHECK(sel.ordered().empty());
    marquee(sel, picks, groups, partial, MarqueeMode::Touching);
    CHECK(sel.ordered() == std::vector<Target>{id(doc, entity)});
    marquee(sel, picks, groups, complete, MarqueeMode::Enclosed);
    CHECK(sel.ordered() == std::vector<Target>{id(doc, entity)});
}

TEST_CASE("Selection select all none invert respect mode hidden wrappers editor flags and isolate") {
    Document doc(parsed());
    const auto world = addBlock(doc, "world { classname worldspawn }");
    const auto group = addBlock(doc, "group {}", world);
    addBlock(doc, "group {}", world); // Empty groups never enter the selectable universe.
    const auto entity = addBlock(doc, "entity { classname func_detail }");
    const auto a = addBox(doc, DVec3{-8}, DVec3{8}, entity);
    const auto b = addBox(doc, DVec3{20}, DVec3{30}, world);
    const auto point = addBlock(doc, "entity { classname info_target origin \"50 0 0\" }");
    join(doc, entity, group);
    join(doc, point, group);
    const auto hidden = addBlock(doc, "hidden {}");
    addBox(doc, DVec3{100}, DVec3{110}, hidden);
    addBlock(doc, "entity { classname hidden editor { visgroupshown 0 } }");
    addBlock(doc, "entity { classname hidden editor { visgroupautoshown 0 } }");
    PickIndex picks(doc);
    GroupIndex groups(doc);
    Selection sel;
    selectAll(sel, picks, groups);
    CHECK(sel.ordered() == std::vector<Target>{id(doc, group), id(doc, b)});
    selectNone(sel);
    CHECK(sel.ordered().empty());
    sel.setMode(SelectionMode::Objects);
    sel.selectAll(doc, picks);
    CHECK(sel.ordered() == std::vector<Target>{id(doc, entity), id(doc, b), id(doc, point)});
    sel.select(id(doc, b));
    sel.invert(doc, picks);
    CHECK(sel.ordered() == std::vector<Target>{id(doc, entity), id(doc, point)});
    invert(sel, picks, groups);
    CHECK(sel.ordered() == std::vector<Target>{id(doc, b)});
    sel.setMode(SelectionMode::Solids);
    selectAll(sel, picks, groups);
    CHECK(sel.ordered() == std::vector<Target>{id(doc, a), id(doc, b)});
    sel.setMode(SelectionMode::Faces);
    selectAll(sel, picks, groups);
    CHECK(sel.ordered().size() == 12);
    ViewState view;
    view.isolate = std::unordered_set<NodeId>{id(doc, entity)};
    picks.setViewState(view);
    selectAll(sel, picks, groups);
    CHECK(sel.ordered().size() == 6);
    sel.setMode(SelectionMode::Objects);
    selectAll(sel, picks, groups);
    CHECK(sel.ordered() == std::vector<Target>{id(doc, entity)});
    CHECK_FALSE(hover(picks, groups, {{-20, 100, 100}, {1, 0, 0}}, SelectionMode::Objects));
    view.isolate->clear();
    picks.setViewState(view);
    selectAll(sel, picks, groups);
    CHECK(sel.ordered().empty());
}

TEST_CASE("Selection undo redo restores stable IDs before and after deletion and prunes nodes and sides") {
    Document doc(parsed());
    const auto a = addBox(doc, DVec3{-8}, DVec3{8});
    const auto b = addBox(doc, DVec3{20}, DVec3{30});
    const auto aId = id(doc, a), bId = id(doc, b);
    Selection sel(SelectionMode::Solids);
    sel.add(aId);
    sel.add(bId);
    const Selection before = sel;
    Selection after(SelectionMode::Objects);
    after.setIgnoreGroups(true);
    auto tx = doc.begin("delete selection");
    tx.destroy(a);
    tx.destroy(b);
    recordSelection(tx, before, after);
    tx.commit();
    sel = after;
    REQUIRE(doc.undo());
    REQUIRE(restoreFromUndo(doc, sel));
    CHECK(sel.toBlob() == before.toBlob());
    CHECK(*doc.handle(aId) != a);
    CHECK(*doc.handle(bId) != b);
    REQUIRE(doc.redo());
    REQUIRE(restoreFromUndo(doc, sel));
    CHECK(sel.toBlob() == after.toBlob());
    REQUIRE(doc.undo());
    REQUIRE(restoreFromUndo(doc, sel));
    PickIndex picks(doc);
    Selection faces(SelectionMode::Faces);
    faces.selectAll(doc, picks);
    REQUIRE(faces.ordered().size() == 12);
    auto erase = doc.begin("delete without metadata");
    erase.destroy(*doc.handle(aId));
    erase.commit();
    sel.prune(doc);
    faces.prune(doc);
    CHECK(sel.ordered() == std::vector<Target>{bId});
    CHECK(sel.active() == Target{bId});
    CHECK(faces.ordered().size() == 6);
    REQUIRE(doc.undo());
    CHECK_FALSE(restoreFromUndo(doc, sel));
    auto metadata = doc.begin("metadata alone");
    const auto undoCount = doc.undoCount();
    recordSelection(metadata, before, after);
    CHECK(metadata.commit().empty());
    CHECK(doc.undoCount() == undoCount);
}

TEST_CASE("Selection malformed visible group member vetoes enclosed and hidden clicks never select") {
    Document doc(parsed());
    const auto group = addBlock(doc, "group {}");
    const auto good = addBox(doc, DVec3{-8}, DVec3{8});
    const auto bad = addBlock(doc, "solid { side { plane broken } }");
    join(doc, good, group);
    join(doc, bad, group);
    const auto hidden = addBlock(doc, "hidden {}");
    addBox(doc, {20, -8, -8}, {30, 8, 8}, hidden);
    addBlock(doc, "entity { classname info_target origin \"40 0 0\" editor { visgroupshown 0 } }");
    PickIndex picks(doc);
    GroupIndex groups(doc);
    Selection sel;
    const auto volume = planes(DVec3{-100}, DVec3{100});
    marquee(sel, picks, groups, volume, MarqueeMode::Enclosed);
    CHECK(sel.ordered().empty());
    marquee(sel, picks, groups, volume, MarqueeMode::Touching);
    CHECK(sel.ordered() == std::vector<Target>{id(doc, group)});
    sel.setMode(SelectionMode::Objects);
    selectAll(sel, picks, groups);
    CHECK(sel.ordered() == std::vector<Target>{id(doc, good), id(doc, bad)});
    click(sel, picks, groups, {{10, 0, 0}, {1, 0, 0}});
    CHECK(sel.ordered().empty());
    CHECK_FALSE(hover(picks, groups, {{10, 0, 0}, {1, 0, 0}}, SelectionMode::Objects));
    auto tx = doc.begin("hide malformed member");
    tx.setEditor(bad, EditorEdit::setVisgroupShown(false));
    tx.commit();
    picks.rebuild();
    groups.rebuild();
    sel.setMode(SelectionMode::Groups);
    marquee(sel, picks, groups, volume, MarqueeMode::Enclosed);
    CHECK(sel.ordered() == std::vector<Target>{id(doc, group)});
}

TEST_CASE("Selection undo helper prunes stale after metadata in face mode") {
    Document doc(parsed());
    const auto a = addBox(doc, DVec3{-8}, DVec3{8});
    addBox(doc, DVec3{20}, DVec3{30});
    PickIndex picks(doc);
    Selection sel(SelectionMode::Faces);
    sel.selectAll(doc, picks);
    const Selection before = sel;
    auto tx = doc.begin("delete with stale after metadata");
    tx.destroy(a);
    recordSelection(tx, before, sel);
    tx.commit();
    REQUIRE(doc.undo());
    REQUIRE(restoreFromUndo(doc, sel));
    CHECK(sel.ordered().size() == 12);
    REQUIRE(doc.redo());
    REQUIRE(restoreFromUndo(doc, sel));
    CHECK(sel.ordered().size() == 6);
    for (const auto target : sel.ordered()) CHECK(doc.side(std::get<SideId>(target)));
}
