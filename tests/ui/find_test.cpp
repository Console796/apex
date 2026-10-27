#include "test_app.hpp"
#include <doctest/doctest.h>

#include "../editor/fixtures.hpp"
#include "ui/find_panel.hpp"
#include "viewport/layout.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTabWidget>
#include <QTreeWidget>
#include <QApplication>
#include <QTest>

using namespace apex;

TEST_CASE("Find panel reports filtered entities and synchronizes row selection") {
    ui_test::app();
    document::Document doc(editor_test::parsed(R"(
world { classname worldspawn }
entity { classname prop_dynamic targetname "Door_A" }
entity { classname prop_static targetname "Door_B" }
entity { classname logic_relay targetname "Relay" }
)"));
    editor::EditorSession session; session.attach(doc);
    ui::FindPanel panel;
    panel.setDocument(&doc); panel.setEditorSession(&session);
    auto* table = panel.entityTable();
    REQUIRE(table->rowCount() == 3);
    auto* classFilter = panel.findChild<QLineEdit*>("findClass");
    REQUIRE(classFilter);
    classFilter->setText("prop_*");
    REQUIRE(table->rowCount() == 2);
    table->selectRow(0);
    QCoreApplication::processEvents();
    REQUIRE(session.selection().ordered().size() == 1);
    CHECK(std::get<document::NodeId>(session.selection().ordered()[0]) == doc.roots()[1]);
    classFilter->clear();
    CHECK(table->rowCount() == 3);
    auto* modes = panel.findChild<QTabWidget*>("findModes");
    REQUIRE(modes);
    modes->setCurrentIndex(1);
    auto* find = panel.findChild<QLineEdit*>("keyReplaceFind");
    REQUIRE(find);
    auto* apply = panel.keyApplyButton();
    CHECK_FALSE(apply->isEnabled());
    find->setText("Door");
    panel.keyPreviewButton()->click();
    REQUIRE(apply->isEnabled());
    CHECK(panel.keyPreviewTable()->rowCount() == 2);
    find->setText("Relay");
    CHECK_FALSE(apply->isEnabled());
    CHECK(panel.keyPreviewTable()->rowCount() == 0);

    // New role names round-trip through the persisted split-tree representation.
    auto layout = viewport::ViewportLayout{};
    const auto pane = layout.paneIds().front();
    REQUIRE(layout.setRole(pane, viewport::AreaRole::Find));
    auto restored = viewport::ViewportLayout::fromJson(layout.toJson());
    REQUIRE(restored);
    CHECK(restored->role(pane) == viewport::AreaRole::Find);
}

TEST_CASE("Find Problems lists missing material references and double-click selects its solid") {
    ui_test::app();
    document::Document doc(editor_test::parsed("world { classname worldspawn }"));
    const auto handle = editor_test::addBox(doc, {-16,-16,-16}, {16,16,16});
    const auto solid = doc.get(handle)->id;
    const auto* node = doc.get(handle);
    REQUIRE(node);
    const auto side = std::ranges::find_if(node->block->children(), [](const kv1::Node& entry) {
        return entry.key() == "side";
    });
    REQUIRE(side != node->block->children().end());
    const auto sideId = static_cast<document::SideId>(side->find("id")->asInt().value());
    {
        auto tx = doc.begin("fixture material");
        tx.setSideKeyValue(sideId, "material", "missing/fixture");
        tx.commit(false);
    }
    editor::EditorSession session; session.attach(doc);
    ui::FindPanel panel;
    panel.setDocument(&doc); panel.setEditorSession(&session);
    auto* tree = panel.problemsTree();
    REQUIRE(tree->topLevelItemCount() >= 1);
    auto* missing = tree->topLevelItem(0);
    REQUIRE(missing->childCount() == 1);
    CHECK(missing->child(0)->text(0).contains("missing/fixture"));
    REQUIRE(missing->child(0)->childCount() == 1);
    auto* reference = missing->child(0)->child(0);
    tree->itemDoubleClicked(reference, 0);
    REQUIRE(session.selection().ordered().size() == 1);
    CHECK(std::get<document::NodeId>(session.selection().ordered()[0]) == solid);
    CHECK(session.selection().mode() == editor::SelectionMode::Objects);
}

TEST_CASE("Find material replace previews and applies selected faces as one undo step") {
    ui_test::app();
    document::Document doc(editor_test::parsed("world { classname worldspawn }"));
    const auto first = editor_test::addBox(doc, {-16,-16,-16}, {16,16,16});
    const auto second = editor_test::addBox(doc, {64,-16,-16}, {96,16,16});
    const auto sidesOf = [&](document::NodeHandle solid) {
        std::vector<document::SideId> result;
        for (const auto& side : doc.get(solid)->block->children()) if (side.key() == "side")
            result.push_back(static_cast<document::SideId>(side.find("id")->asInt().value()));
        return result;
    };
    const auto firstSides = sidesOf(first), secondSides = sidesOf(second);
    {
        auto tx = doc.begin("fixture materials");
        for (auto side : firstSides) tx.setSideKeyValue(side, "material", "fixture/wall");
        for (auto side : secondSides) tx.setSideKeyValue(side, "material", "fixture/wall");
        tx.commit(false);
    }
    editor::EditorSession session; session.attach(doc);
    session.selection().setMode(editor::SelectionMode::Faces);
    session.selection().add(firstSides[0]); session.selection().add(firstSides[1]);
    const auto before = doc.write().value();
    const auto history = doc.undoCount();
    ui::FindPanel panel;
    panel.setDocument(&doc); panel.setEditorSession(&session);
    auto* modes = panel.findChild<QTabWidget*>("findModes");
    REQUIRE(modes); modes->setCurrentIndex(1);
    auto* find = panel.findChild<QLineEdit*>("materialReplaceFind");
    auto* replacement = panel.findChild<QLineEdit*>("materialReplaceWith");
    REQUIRE(find); REQUIRE(replacement);
    find->setText("fixture/wall"); replacement->setText("fixture/floor");
    auto* apply = panel.materialApplyButton();
    CHECK_FALSE(apply->isEnabled());
    panel.materialPreviewButton()->click();
    REQUIRE(apply->isEnabled());
    CHECK(panel.materialPreviewTable()->rowCount() == 2);
    // Changing a preview input discards the reviewed change list.
    replacement->setText("fixture/stone");
    CHECK_FALSE(apply->isEnabled());
    CHECK(panel.materialPreviewTable()->rowCount() == 0);
    panel.materialPreviewButton()->click();
    REQUIRE(apply->isEnabled());
    const auto expected = panel.materialPreviewTable()->item(0, 2)->text();
    apply->click();
    CHECK(doc.undoCount() == history + 1);
    const auto* changed = doc.get(first)->block.get();
    const auto side = std::ranges::find_if(changed->children(), [&](const kv1::Node& entry) {
        return entry.key() == "side" && entry.find("id")->asInt() == firstSides[0];
    });
    REQUIRE(side != changed->children().end());
    CHECK(QString::fromUtf8(side->find("material")->decodedValue().value().c_str()) == expected);
    const auto untouched = std::ranges::find_if(doc.get(second)->block->children(), [&](const kv1::Node& entry) {
        return entry.key() == "side" && entry.find("id")->asInt() == secondSides[0];
    });
    REQUIRE(untouched != doc.get(second)->block->children().end());
    CHECK(untouched->find("material")->decodedValue() == "fixture/wall");
    REQUIRE(doc.undo());
    CHECK(doc.write().value() == before);
}
