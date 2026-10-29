#include "test_app.hpp"
#include "../assets/fixture.hpp"
#include "../assets/texture_fixture.hpp"
#include "assets/material_cache.hpp"
#include "../editor/fixtures.hpp"
#include "assets/vfs.hpp"
#include "core/json.hpp"
#include "ui/addons_panel.hpp"
#include "ui/find_panel.hpp"
#include "ui/game_content.hpp"
#include "ui/area_frame.hpp"
#include "ui/viewport_area.hpp"
#include "viewport/layout.hpp"

#include <doctest/doctest.h>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTest>
#include <QTreeWidget>
#include <cstdlib>

using namespace apex;
namespace {
constexpr std::string_view header = "<!-- kv3 encoding:text:version{apex} format:generic:version{fixture} -->\r\n";
std::string bytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), {});
}
void finish(ui::GameContent& content) {
    for (int i = 0; i < 15000 && (!content.snapshot() || !content.modelSnapshot() ||
         !content.soundSnapshot() || !content.particleSnapshot()); ++i) QTest::qWait(1);
    REQUIRE_MESSAGE(content.vfs(), content.error().toStdString());
    REQUIRE(content.snapshot()); REQUIRE(content.modelSnapshot());
    REQUIRE(content.soundSnapshot()); REQUIRE(content.particleSnapshot());
}
struct AddonsFixture {
    GameTree tree;
    assets::GameProfile profile;
    std::string original;
    AddonsFixture() {
        tree.put("Game/gameinfo.txt", "GameInfo { FileSystem { SearchPaths { game |gameinfo_path|. } } }");
        for (const auto name : {"Local", "Workshop"}) {
            tree.put(std::string(name) + "/addon.kv3", std::string(header) + R"({ mod = "Fixture pack" id = 42 type = "Asset Pack" authors = ["Apex"] tags = ["Models", "Materials/Textures"] thumbnail = ".assets/thumb.png" })");
        }
        tree.put("Local/.assets/placeholder", "");
        QImage image(8, 8, QImage::Format_RGBA8888); image.fill(Qt::red);
        REQUIRE(image.save(QString::fromStdString((tree.root / "Local/.assets/thumb.png").string())));
        tree.put("Local/materials/fixture/only.vmt", "UnlitGeneric { $basetexture fixture/only }");
        auto texture = whiteVtf();
        REQUIRE(texture.bake(tree.root / "Local/materials/fixture/only.vtf"));
        tree.put("Local/models/only.mdl", "directory fixture");
        const auto entry = [&](const char* name, const char* source) {
            return "\t{ dir = " + json::write({(tree.root / name).string()}) +
                " mounted = false type = \"" + source + "\" vpks = [] }\r\n";
        };
        original = std::string(header) + "{\r\naddons = [\r\n" + entry("Local", "local") + entry("Workshop", "steam") + entry("Missing", "local") + "]\r\n}\r\n";
        tree.put("hammer/addons/addonlist.kv3", original);
        profile.gameDirectory = tree.root / "Game";
        profile.addons.list = assets::AddonSettings::List::Hammer;
    }
    auto file() const { return tree.root / "hammer/addons/addonlist.kv3"; }
};
}
TEST_CASE("Addons panel lists metadata warnings thumbnail filter and restart notice") {
    ui_test::app();
    AddonsFixture fixture;
    ui::GameContent content; content.reload(&fixture.profile); finish(content);
    ui::AddonsPanel panel; panel.setGameContent(&content); panel.show();
    auto* rows = panel.findChild<QTreeWidget*>("addonsEntries"); REQUIRE(rows);
    REQUIRE(rows->topLevelItemCount() == 3);
    auto* local = rows->topLevelItem(0);
    CHECK(local->text(0) == "Fixture pack"); CHECK(local->text(1) == "Apex");
    CHECK(local->text(2) == "Asset Pack"); CHECK(local->text(3) == "Local");
    CHECK(local->text(4) == "42"); CHECK(local->text(5).contains("Models"));
    CHECK(local->text(6).contains("Duplicate Workshop ID"));
    CHECK_FALSE(local->icon(0).isNull());
    CHECK(rows->topLevelItem(1)->text(3) == "Workshop");
    CHECK(rows->topLevelItem(1)->text(6).contains("Duplicate Workshop ID"));
    CHECK(rows->topLevelItem(2)->text(6).contains("Cannot resolve"));
    auto* filter = panel.findChild<QLineEdit*>("addonsFilter"); REQUIRE(filter);
    filter->setText("Missing"); CHECK(local->isHidden());
    CHECK_FALSE(rows->topLevelItem(2)->isHidden());
    filter->clear(); CHECK_FALSE(local->isHidden());
    auto* notice = panel.findChild<QLabel*>("addonsRestartNotice"); REQUIRE(notice);
    CHECK(notice->isVisible());
    CHECK(notice->text() == "Hammer reads the addon list when it starts; restart Hammer for a change to apply");
    local->setCheckState(0, Qt::Checked);
    QTest::qWait(20); finish(content);
    CHECK(content.assetExists("materials/fixture/only.vmt"));
    auto expected = fixture.original; expected.replace(expected.find("mounted = false"), 15, "mounted = true");
    CHECK(bytes(fixture.file()) == expected);
    auto layout = viewport::ViewportLayout{}; const auto pane = layout.paneIds().front();
    REQUIRE(layout.setRole(pane, viewport::AreaRole::Addons));
    auto restored = viewport::ViewportLayout::fromJson(layout.toJson()); REQUIRE(restored);
    CHECK(restored->role(pane) == viewport::AreaRole::Addons);
    CHECK(viewport::roleFromName("addons") == viewport::AreaRole::Addons);
    std::unique_ptr<QWidget> role(ui::createPanel(viewport::AreaRole::Addons, nullptr, nullptr, &content));
    CHECK(qobject_cast<ui::AddonsPanel*>(role.get()));
}
TEST_CASE("Addons Problems Mount names an unmounted provider and preserves list bytes") {
    ui_test::app(); AddonsFixture fixture;
    ui::GameContent content; content.reload(&fixture.profile); finish(content);
    CHECK(content.missingAssetTooltip("materials/fixture/only.vmt").contains("Fixture pack"));
    CHECK(content.missingAssetTooltip("models/only.mdl").contains("Fixture pack"));
    document::Document doc(editor_test::parsed(R"(world { classname worldspawn } entity { classname prop_static origin "100 0 0" model models/only.mdl })"));
    auto solid = editor_test::addBox(doc, {-16,-16,-16}, {16,16,16});
    std::vector<document::SideId> sides;
    for (const auto& side : doc.get(solid)->block->children()) if (side.key() == "side")
        sides.push_back(*side.find("id")->asInt());
    auto tx = doc.begin("fixture material");
    for (const auto side : sides) tx.setSideKeyValue(side, "material", "fixture/only");
    tx.commit(false);
    editor::EditorSession session; session.attach(doc);
    ui::ViewportArea area;
    area.setGameContent(&content); area.setEditorSession(&session); area.setDocument(&doc, &fixture.profile);
    CHECK(area.missingAssetTooltip({{0,-100,0}, {0,1,0}}).contains("Fixture pack"));
    CHECK(area.missingAssetTooltip({{100,-100,0}, {0,1,0}}).contains("Fixture pack"));
    ui::FindPanel panel; panel.setGameContent(&content); panel.setDocument(&doc); panel.setEditorSession(&session);
    auto* missing = panel.problemsTree()->topLevelItem(0); REQUIRE(missing->childCount() == 2);
    CHECK(missing->child(0)->text(1).contains("Fixture pack"));
    auto* mount = panel.findChild<QPushButton*>("mountMissingAddon"); REQUIRE(mount);
    const auto mapBefore = doc.write().value();
    mount->click(); QTest::qWait(20); finish(content);
    CHECK(doc.write().value() == mapBefore);
    auto expected = fixture.original; expected.replace(expected.find("mounted = false"), 15, "mounted = true");
    CHECK(bytes(fixture.file()) == expected);
    CHECK(content.assetExists("materials/fixture/only.vmt"));
    CHECK(content.missingAssetTooltip("materials/fixture/only.vmt").isEmpty());
    auto mountedVfs = content.vfs();
    JobPool jobs(1); assets::MaterialCache cache(*mountedVfs, jobs);
    cache.request("fixture/only");
    for (int i = 0; i < 5000 && cache.state("fixture/only") == assets::MaterialState::Loading; ++i) {
        jobs.drainCompletions(); QTest::qWait(1);
    }
    CHECK(cache.state("fixture/only") == assets::MaterialState::Ready);
    CHECK(panel.problemsTree()->topLevelItem(0)->childCount() == 0);
    auto* used = panel.findChild<QTreeWidget*>("findAddons"); REQUIRE(used);
    REQUIRE(used->topLevelItemCount() == 1); CHECK(used->topLevelItem(0)->text(0) == "Fixture pack");
}
TEST_CASE("Addons watcher reloads in-place edits and repeated atomic replacements") {
    ui_test::app(); AddonsFixture fixture;
    ui::GameContent content; content.reload(&fixture.profile); finish(content);
    ui::AddonsPanel panel; panel.setGameContent(&content);
    for (int edit = 0; edit < 3; ++edit) {
        const bool mounted = edit % 2 == 0;
        auto changed = fixture.original;
        if (mounted) changed.replace(changed.find("mounted = false"), 15, "mounted = true");
        if (edit == 0) fixture.tree.put("hammer/addons/addonlist.kv3", changed);
        else {
            fixture.tree.put("hammer/addons/replacement", changed);
            std::filesystem::rename(fixture.file().parent_path() / "replacement", fixture.file());
        }
        for (int i = 0; i < 5000 && content.assetExists("materials/fixture/only.vmt") != mounted; ++i) QTest::qWait(1);
        finish(content);
        CHECK(content.assetExists("materials/fixture/only.vmt") == mounted);
        auto* rows = panel.findChild<QTreeWidget*>("addonsEntries"); REQUIRE(rows);
        REQUIRE(rows->topLevelItemCount() == 3);
        CHECK(rows->topLevelItem(0)->checkState(0) == (mounted ? Qt::Checked : Qt::Unchecked));
    }
    const auto previous = content.vfs();
    fixture.tree.put("hammer/addons/addonlist.kv3", "incomplete save");
    for (int i = 0; i < 5000 && content.vfs() == previous; ++i) QTest::qWait(1);
    finish(content);
    REQUIRE(content.vfs()->addons().entries.empty());
    fixture.tree.put("hammer/addons/replacement", fixture.original);
    std::filesystem::rename(fixture.file().parent_path() / "replacement", fixture.file());
    for (int i = 0; i < 5000 && (!content.vfs() || content.vfs()->addons().entries.empty()); ++i) QTest::qWait(1);
    finish(content);
    CHECK(content.vfs()->addons().entries.size() == 3);
}
TEST_CASE("Addons panel names the missing Workshop Uploader executable") {
    ui_test::app(); AddonsFixture fixture;
    ui::GameContent content; content.reload(&fixture.profile); finish(content);
    ui::AddonsPanel panel; panel.setGameContent(&content);
    auto* uploader = panel.findChild<QPushButton*>("addonsUploader"); REQUIRE(uploader);
    auto* status = panel.findChild<QLabel*>("addonsStatus"); REQUIRE(status);
    const auto expected = QString::fromStdString(fixture.profile.toolPath("workshopgui").string());
    uploader->click();
    CHECK(status->text().contains("Workshop Uploader not found"));
    CHECK(status->text().contains(expected));
}
TEST_CASE("Addons panel corpus lists the followed install addons read-only") {
    ui_test::app();
    const auto* install = std::getenv("APEX_P2CE_DIR");
    if (!install || !*install) { MESSAGE("Skipped: APEX_P2CE_DIR is unset"); return; }
    assets::GameProfile profile; profile.gameDirectory = std::filesystem::canonical(install) / "p2ce";
    auto list = assets::readAddonList(profile); REQUIRE(list);
    ui::GameContent content; content.reload(&profile); finish(content);
    ui::AddonsPanel panel; panel.setGameContent(&content);
    auto* rows = panel.findChild<QTreeWidget*>("addonsEntries"); REQUIRE(rows);
    REQUIRE(rows->topLevelItemCount() == static_cast<int>(list->entries.size()));
    for (int i = 0; i < rows->topLevelItemCount(); ++i) {
        const auto& entry = list->entries[static_cast<std::size_t>(i)];
        CHECK(rows->topLevelItem(i)->text(0).toStdString() == entry.name);
        for (const auto& problem : entry.problems)
            CHECK(rows->topLevelItem(i)->text(6).contains(QString::fromStdString(problem)));
    }
}
