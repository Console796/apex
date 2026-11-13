#include "fixtures.hpp"
#include "editor/editor_session.hpp"
#include "core/strings.hpp"

#include <set>
#include <sstream>

using namespace editor_test;

// 0.8 D11 rule 1: every copy path remaps FGD sidelist keys through the pasted side-ID map.
namespace {

using Kv = kv1::Node;

assets::EntityClassTable overlayClasses() {
    assets::EntityClassTable table;
    assets::EntityClass overlay;
    overlay.name = "info_overlay";
    assets::EntityProperty sides;
    sides.name = "sides";
    sides.type = "sidelist";
    overlay.properties.push_back(std::move(sides));
    table.classes["info_overlay"] = std::move(overlay);
    return table;
}

std::vector<int> sidesOf(const Document& doc, NodeId solid) {
    std::vector<int> out;
    for (const auto& child : doc.exportSubtree(solid).children())
        if (child.kind() == kv1::NodeKind::Block && child.key() == "side") out.push_back(child.find("id")->asInt().value());
    return out;
}

std::vector<int> listed(const Kv& entity) {
    std::vector<int> out;
    const std::string text = entity.find("sides")->decodedValue().value_or("");
    for (const auto word : split(text, ' ')) out.push_back(Kv::value("", std::string(word)).asInt().value());
    return out;
}

struct Scene {
    Document doc{parsed("versioninfo\n{\n\"editorversion\" \"400\"\n}\nworld\n{\n\"id\" \"1\"\n\"classname\" \"worldspawn\"\n}\n")};
    EditorSession session;
    NodeHandle world;
    NodeId copied = 0, kept = 0, overlay = 0;
    std::vector<int> copiedSides, keptSides;

    Scene() {
        for (const auto r : doc.roots())
            if (doc.get(*doc.handle(r))->block->key() == "world") world = *doc.handle(r);
        session.attach(doc);
        session.setEntityClasses(overlayClasses());
        session.perform("selection.mode.objects");
        copied = id(doc, addBox(doc, {0, 0, 0}, {64, 64, 64}, world));
        kept = id(doc, addBox(doc, {256, 0, 0}, {320, 64, 64}, world));
        copiedSides = sidesOf(doc, copied);
        keptSides = sidesOf(doc, kept);
        REQUIRE(copiedSides.size() == 6);
        std::ostringstream text;
        text << "entity\n{\n\"id\" \"0\"\n\"classname\" \"info_overlay\"\n\"origin\" \"64 32 32\"\n\"sides\" \""
             << copiedSides[4] << ' ' << copiedSides[0] << ' ' << keptSides[1] << ' ' << copiedSides[2] << "\"\n}";
        overlay = id(doc, addBlock(doc, text.str()));
        session.selection().clear();
        session.selection().add(copied);
        session.selection().add(overlay);
    }
    std::vector<NodeId> newSolids() const {
        std::vector<NodeId> out;
        for (const auto c : doc.get(world)->children)
            if (std::holds_alternative<Handle<Solid>>(*doc.handle(c)) && c != copied && c != kept) out.push_back(c);
        return out;
    }
    std::vector<Kv> newOverlays() const {
        std::vector<Kv> out;
        for (const auto r : doc.roots()) {
            if (r == overlay || !std::holds_alternative<Handle<Entity>>(*doc.handle(r))) continue;
            auto block = doc.exportSubtree(r);
            if (block.key() == "entity" && block.find("classname") &&
                block.find("classname")->decodedValue().value_or("") == "info_overlay")
                out.push_back(std::move(block));
        }
        return out;
    }
    // Each new overlay lists its own copy's faces in the original order; the uncopied face stays.
    void checkCopies(std::size_t copies) {
        const auto solids = newSolids();
        const auto overlays = newOverlays();
        REQUIRE(solids.size() == copies);
        REQUIRE(overlays.size() == copies);
        std::set<NodeId> matched;
        for (const auto& entity : overlays) {
            const auto ids = listed(entity);
            CAPTURE(entity.find("sides")->decodedValue().value_or(""));
            bool found = false;
            for (const auto solid : solids) {
                const auto s = sidesOf(doc, solid);
                REQUIRE(s.size() == 6);
                if (ids == std::vector<int>{s[4], s[0], keptSides[1], s[2]}) { matched.insert(solid); found = true; }
            }
            CHECK(found);
        }
        CHECK(matched.size() == copies);
        // The original is untouched.
        Kv original = doc.exportSubtree(overlay);
        CHECK(listed(original) == std::vector<int>{copiedSides[4], copiedSides[0], keptSides[1], copiedSides[2]});
    }
};

} // namespace

TEST_CASE("[editor][sidelist] duplicate remaps overlay sides to the copied faces") {
    Scene s;
    const auto undo = s.doc.undoCount();
    const auto before = s.doc.write().value();
    REQUIRE(s.session.duplicateSelection());
    CHECK(s.doc.undoCount() == undo + 1);
    s.checkCopies(1);
    // One undo step restores the copy and its remapped key together.
    REQUIRE(s.session.perform("edit.undo"));
    CHECK(s.doc.write().value() == before);
}

TEST_CASE("[editor][sidelist] copy and paste remaps overlay sides to the pasted faces") {
    Scene s;
    const auto text = s.session.copyText();
    REQUIRE(text);
    REQUIRE(s.session.pasteText(*text));
    s.checkCopies(1);
}

TEST_CASE("[editor][sidelist] paste special remaps each copy's overlay to that copy's faces") {
    Scene s;
    const auto text = s.session.copyText();
    REQUIRE(text);
    PasteOptions options;
    options.inPlace = true;
    PasteSpecial special;
    special.copies = 2;
    special.offset = {0, 128, 0};
    options.special = special;
    REQUIRE(s.session.pasteText(*text, options));
    s.checkCopies(2);
}
