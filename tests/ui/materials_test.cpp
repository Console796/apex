#include "test_app.hpp"
#include "../assets/fixture.hpp"
#include "../assets/texture_fixture.hpp"
#include "../editor/fixtures.hpp"

#include <doctest/doctest.h>
#include <chrono>
#include <cstdlib>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QKeyEvent>
#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QPixmap>
#include <QMouseEvent>
#include <QPointer>
#include <QLineEdit>
#include <QListView>
#include <QSettings>
#include <QThread>
#include <QLabel>
#include <QCheckBox>
#include <QPushButton>
#include <QToolButton>
#include <QMenu>
#include <QSlider>
#include <QStatusBar>

#include "document/document.hpp"
#include "document/vmf.hpp"
#include "ui/area_frame.hpp"
#include "ui/game_content.hpp"
#include "ui/inspector_panel.hpp"
#include "ui/main_window.hpp"
#include "ui/materials_panel.hpp"
#include "ui/viewport_area.hpp"
#include "viewport/layout.hpp"

using namespace apex;
namespace {
QApplication& app() { return apex::ui_test::app(); }
void waitForIndex(ui::GameContent& content) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!content.snapshot() && std::chrono::steady_clock::now() < deadline) {
        app().processEvents();
        QThread::msleep(1);
    }
    REQUIRE(content.snapshot());
}
assets::GameProfile makeProfile(GameTree& tree) {
    tree.put("Game/gameinfo.txt", "GameInfo { FileSystem { SearchPaths { game |gameinfo_path|. } } }");
    tree.put("Game/materials/brick.vmt", "LightmappedGeneric { $basetexture white %keywords \"metal, wall\" }");
    tree.put("Game/materials/target.vmt", "VertexLitGeneric { $basetexture white %keywords \"painted\" }");
    tree.put("Game/materials/other.vmt", "UnlitGeneric { $basetexture white }");
    tree.put("Game/materials/tools/clip.vmt", "UnlitGeneric { $basetexture white }");
    auto texture = whiteVtf();
    REQUIRE(texture.bake(tree.root / "Game/materials/white.vtf"));
    tree.put("Game/game.fgd", R"(@PointClass = info_target [
        material(material) : "Material" : "brick"
    ]
)");
    assets::GameProfile profile;
    profile.name = "Materials test";
    profile.gameDirectory = tree.root / "Game";
    profile.fgds.push_back(tree.root / "Game/game.fgd");
    return profile;
}
document::SideId firstSide(const document::Document& doc, document::NodeId solid) {
    const auto* node = doc.get(*doc.handle(solid));
    for (const auto& child : node->block->children())
        if (child.kind() == kv1::NodeKind::Block && ciEquals(child.key(), "side"))
            return static_cast<document::SideId>(*child.find("id")->asInt());
    return -1;
}
std::string sideMaterial(const document::Document& doc, document::SideId side) {
    const auto location = doc.side(side);
    REQUIRE(location);
    const auto* node = doc.get(location->solid);
    std::size_t index = 0;
    for (const auto& child : node->block->children()) {
        if (child.kind() != kv1::NodeKind::Block || !ciEquals(child.key(), "side")) continue;
        if (index++ != location->faceIndex) continue;
        return child.find("material")->decodedValue().value_or("");
    }
    return {};
}
}

TEST_CASE("Materials role is listed and layouts round-trip it while old layouts load") {
    app();
    using viewport::AreaRole;
    auto layout = viewport::ViewportLayout::quad();
    const auto pane = layout.paneIds().front();
    REQUIRE(layout.setRole(pane, AreaRole::Materials));
    const auto encoded = json::write(layout.toJson());
    const auto decoded = json::parse(encoded);
    REQUIRE(decoded);
    auto restored = viewport::ViewportLayout::fromJson(*decoded);
    REQUIRE(restored);
    CHECK(restored->role(pane) == AreaRole::Materials);
    auto old = viewport::ViewportLayout::quad();
    auto oldDecoded = viewport::ViewportLayout::fromJson(old.toJson());
    REQUIRE(oldDecoded);
    CHECK(oldDecoded->paneIds(AreaRole::Materials).empty());

    ui::MainWindow window;
    auto* show = window.findChild<QAction*>("actionShowArea_materials");
    REQUIRE(show);
    CHECK(window.viewportArea()->viewportLayout().paneIds(AreaRole::Materials).empty());
    auto* roleButton = window.viewportArea()->findChild<QToolButton*>("AreaRoleButton");
    REQUIRE(roleButton);
    bool found = false;
    for (auto* action : roleButton->menu()->actions()) found |= action->data().toString() == "materials";
    CHECK(found);
}

TEST_CASE("GameContent reloads safely during indexing and MaterialsPanel filters and applies") {
    app();
    QSettings settings;
    settings.remove("materials/recent");
    settings.remove("materials/thumbnailSize");
    GameTree tree;
    const auto profile = makeProfile(tree);
    ui::MainWindow window;
    window.setProfiles({profile});
    const auto firstVfs = window.gameContent().vfs();
    REQUIRE(firstVfs);
    window.gameContent().reload(&profile); // Replaces the Vfs while the first walk can still be running.
    REQUIRE(window.gameContent().vfs());
    CHECK(window.gameContent().vfs() != firstVfs);
    waitForIndex(window.gameContent());

    auto parsed = vmf::parse("versioninfo { editorversion 400 mapversion 1 } world { id 1 classname worldspawn }");
    REQUIRE(parsed);
    document::Document doc(*parsed);
    auto world = *doc.handle(doc.roots().front());
    auto solid = editor_test::solidBlock(geometry_test::build(geometry_test::boxSides()));
    for (auto& side : solid.children()) side.append(kv1::Node::value("material", "other")).value();
    auto tx = doc.begin("fixture solid");
    const auto solidHandle = tx.create(std::move(solid), world);
    tx.commit();
    const auto solidId = doc.get(solidHandle)->id;
    const auto side = firstSide(doc, solidId);

    window.editorSession().attach(doc);
    window.editorSession().selection().setMode(editor::SelectionMode::Faces);
    window.editorSession().selection().select(side);
    auto* panel = new ui::MaterialsPanel;
    panel->setGameContent(&window.gameContent());
    panel->setDocument(&doc);
    panel->setEditorSession(&window.editorSession());
    QObject::connect(panel, &ui::MaterialsPanel::activeMaterialRequested, &window, &ui::MainWindow::activateMaterial);
    panel->resize(900, 520);
    panel->show();
    app().processEvents();
    CHECK(panel->filteredNames().contains("brick"));
    auto* filter = panel->findChild<QLineEdit*>("materialsNameFilter");
    auto* shader = panel->findChild<QComboBox*>("materialsShaderFilter");
    auto* used = panel->findChild<QCheckBox*>("materialsUsedFilter");
    auto* tools = panel->findChild<QCheckBox*>("materialsToolFilter");
    auto* grid = panel->findChild<QListView*>("materialsGrid");
    REQUIRE(filter); REQUIRE(shader); REQUIRE(used); REQUIRE(tools); REQUIRE(grid);

    filter->setText("BRICK");
    CHECK(panel->filteredNames() == QStringList{"brick"});
    filter->setText("*/clip");
    CHECK(panel->filteredNames() == QStringList{"tools/clip"});
    filter->setText("keyword:metal");
    CHECK(panel->filteredNames() == QStringList{"brick"});
    filter->clear();
    shader->setCurrentIndex(shader->findData("vertexlitgeneric"));
    CHECK(panel->filteredNames() == QStringList{"target"});
    shader->setCurrentIndex(0);
    filter->setText("tools/clip");
    tools->setChecked(false);
    CHECK(panel->filteredNames().empty());
    tools->setChecked(true);
    filter->clear();
    used->setChecked(true);
    CHECK(panel->filteredNames() == QStringList{"other"});
    used->setChecked(false);
    filter->setText("target");
    auto targetIndex = grid->model()->index(0, 0);
    REQUIRE(targetIndex.isValid());
    grid->clicked(targetIndex);
    CHECK(window.editorSession().newSolidStyle().material == "target");
    CHECK(settings.value("editor/newSolidMaterial").toString() == "target");
    CHECK(settings.value("materials/recent").toStringList().front() == "target");
    auto* indicator = window.findChild<QToolButton*>("activeMaterialIndicator");
    REQUIRE(indicator);
    CHECK(indicator->text().contains("target"));

    targetIndex = grid->model()->index(0, 0);
    grid->doubleClicked(targetIndex);
    CHECK(sideMaterial(doc, side) == "target");
    CHECK(doc.undoLabels().back() == "Apply Material");
    used->setChecked(true);
    filter->clear();
    CHECK(panel->filteredNames().contains("target"));
    doc.undo();
    CHECK(sideMaterial(doc, side) == "other");
    CHECK_FALSE(panel->filteredNames().contains("target"));
    used->setChecked(false);
    const auto undoBeforeEmpty = doc.undoCount();
    window.editorSession().selection().clear();
    filter->setText("target");
    targetIndex = grid->model()->index(0, 0);
    REQUIRE(targetIndex.isValid());
    grid->doubleClicked(targetIndex);
    CHECK(doc.undoCount() == undoBeforeEmpty);

    auto* slider = panel->findChild<QSlider*>("materialsThumbnailSize");
    REQUIRE(slider);
    slider->setValue(96);
    CHECK(settings.value("materials/thumbnailSize").toInt() == 96);
    // Only the filtered visible target is requested; other indexed entries remain unloaded.
    filter->setText("target");
    const auto visible = grid->model()->index(0, 0);
    REQUIRE(visible.isValid());
    grid->model()->data(visible, Qt::DecorationRole);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (window.gameContent().thumbnails()->state("target", 96) == assets::ThumbnailState::Loading &&
           std::chrono::steady_clock::now() < deadline) {
        app().processEvents(); QThread::msleep(1);
    }
    CHECK(window.gameContent().thumbnails()->state("target", 96) == assets::ThumbnailState::Ready);
    CHECK(window.gameContent().thumbnails()->state("other", 96) == assets::ThumbnailState::Unloaded);
    CHECK(panel->thumbnailRequestCount() > 0);

    // A single grid click applies only while the face edit tool is active.
    REQUIRE(window.editorSession().perform("tool.apply"));
    window.editorSession().selection().setMode(editor::SelectionMode::Faces);
    window.editorSession().selection().select(side);
    filter->setText("brick");
    const auto brick = grid->model()->index(0, 0);
    REQUIRE(brick.isValid());
    grid->clicked(brick);
    CHECK(sideMaterial(doc, side) == "brick");
    CHECK(doc.undoLabels().back() == "Apply Material");
    REQUIRE(window.editorSession().perform("tool.select"));
    window.editorSession().selection().setMode(editor::SelectionMode::Faces);
    window.editorSession().selection().select(side);
    const auto undoBeforeActiveOnly = doc.undoCount();
    filter->setText("target");
    const auto target = grid->model()->index(0, 0);
    REQUIRE(target.isValid());
    grid->clicked(target);
    CHECK(window.editorSession().newSolidStyle().material == "target");
    CHECK(sideMaterial(doc, side) == "brick");
    CHECK(doc.undoCount() == undoBeforeActiveOnly);
    delete panel;
    window.editorSession().detach(); // doc is destroyed before the window
}

TEST_CASE("Inspector material browse writes on pick and Esc leaves the key unchanged") {
    app();
    const auto oldBrowserMode = QSettings().value("ui/materialBrowserWindow");
    ui::MainWindow::setMaterialBrowserInWindow(false);
    GameTree tree;
    const auto profile = makeProfile(tree);
    ui::MainWindow window;
    window.setProfiles({profile});
    waitForIndex(window.gameContent());
    window.newFile();
    auto* doc = window.document();
    auto tx = doc->begin("add target");
    auto entity = kv1::Node::block("entity");
    entity.append(kv1::Node::value("classname", "info_target")).value();
    entity.append(kv1::Node::value("material", "brick")).value();
    const auto handle = tx.create(std::move(entity));
    tx.commit();
    const auto id = doc->get(handle)->id;
    window.editorSession().selectNode(id);
    auto* panel = qobject_cast<ui::InspectorPanel*>(window.viewportArea()->showRole(viewport::AreaRole::Inspector, true));
    REQUIRE(panel);
    app().processEvents();
    auto* browse = panel->findChild<QPushButton*>("inspectorBrowse.material");
    REQUIRE(browse);
    CHECK(browse->isEnabled());
    browse->click();
    app().processEvents();
    auto* dialog = window.findChild<QDialog*>("materialPicker");
    REQUIRE(dialog);
    auto* picker = dialog->findChild<ui::MaterialsPanel*>("materialsPanel");
    REQUIRE(picker);
    CHECK(picker->findChild<QLineEdit*>("materialsNameFilter")->text() == "brick");
    picker->materialChosen("target");
    app().processEvents();
    auto* block = doc->get(*doc->handle(id))->block.get();
    CHECK(block->find("material")->decodedValue() == "target");

    browse = panel->findChild<QPushButton*>("inspectorBrowse.material");
    REQUIRE(browse);
    browse->click();
    app().processEvents();
    dialog = window.findChild<QDialog*>("materialPicker");
    REQUIRE(dialog);
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(dialog, &escape);
    app().processEvents();
    CHECK(doc->get(*doc->handle(id))->block->find("material")->decodedValue() == "target");
    if (oldBrowserMode.isValid()) QSettings().setValue("ui/materialBrowserWindow", oldBrowserMode);
    else QSettings().remove("ui/materialBrowserWindow");
}

TEST_CASE("Material Browse uses the Materials area once, cancels with Escape and honors window mode") {
    app();
    GameTree tree;
    const auto profile = makeProfile(tree);
    const auto oldBrowserMode = QSettings().value("ui/materialBrowserWindow");
    ui::MainWindow::setMaterialBrowserInWindow(false);
    {
        ui::MainWindow window;
        window.setProfiles({profile});
        waitForIndex(window.gameContent());
        auto* area = window.viewportArea();
        const auto pane = area->viewportLayout().paneIds().front();
        area->setRole(pane, viewport::AreaRole::Materials);
        area->flush();
        area->setCollapsed(pane, true);
        area->flush();
        window.show();
        QString chosen;
        const auto active = window.editorSession().newSolidStyle().material;
        window.openMaterialPicker("brick", "the selected faces", [&](QString value) { chosen = value; });
        app().processEvents();
        auto* panel = qobject_cast<ui::MaterialsPanel*>(area->panel(pane));
        REQUIRE(panel);
        CHECK_FALSE(area->viewportLayout().collapsed(pane));
        CHECK(panel->findChild<QLabel*>("materialsPickBanner")->text().contains("the selected faces"));
        CHECK(panel->findChild<QLineEdit*>("materialsNameFilter")->text() == "brick");
        auto* grid = panel->findChild<QListView*>("materialsGrid");
        REQUIRE(grid);
        const auto index = grid->model()->index(0, 0);
        REQUIRE(index.isValid());
        const auto expected = index.data().toString();
        grid->clicked(index);
        CHECK(chosen == expected);
        CHECK_FALSE(panel->findChild<QLabel*>("materialsPickBanner")->isVisible());
        CHECK(window.editorSession().newSolidStyle().material == active);

        chosen.clear();
        window.openMaterialPicker("brick", "first browse", [&](QString value) { chosen = value; });
        window.openMaterialPicker("brick", "second browse", [&](QString value) { chosen = value; });
        CHECK(panel->findChild<QLabel*>("materialsPickBanner")->text().contains("second browse"));
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(grid, &escape);
        CHECK(chosen.isEmpty());
        CHECK_FALSE(panel->findChild<QLabel*>("materialsPickBanner")->isVisible());
        CHECK(window.editorSession().newSolidStyle().material == active);

        ui::MainWindow::setMaterialBrowserInWindow(true);
        window.openMaterialPicker("brick", "material", [](QString) {});
        CHECK(window.findChild<QDialog*>("materialPicker"));
    }
    if (oldBrowserMode.isValid()) QSettings().setValue("ui/materialBrowserWindow", oldBrowserMode);
    else QSettings().remove("ui/materialBrowserWindow");
}

TEST_CASE("Materials grid: a real double-click applies a new material, rows drag, and panels die cleanly") {
    app();
    GameTree tree;
    const auto profile = makeProfile(tree);
    QPointer<ui::MaterialsPanel> docked;
    {
        ui::MainWindow window;
        window.setProfiles({profile});
        waitForIndex(window.gameContent());
        window.newFile();
        auto* doc = window.document();
        const auto box = editor_test::addBox(*doc, {0, 0, 0}, {64, 64, 64});
        const auto side = firstSide(*doc, doc->get(box)->id);
        window.editorSession().selection().setMode(editor::SelectionMode::Faces);
        window.editorSession().selection().select(side);
        window.editorSession().setNewSolidMaterial("other");

        docked = qobject_cast<ui::MaterialsPanel*>(window.viewportArea()->showRole(viewport::AreaRole::Materials, false));
        REQUIRE(docked);
        window.resize(1200, 800);
        window.show();
        app().processEvents();
        auto* filter = docked->findChild<QLineEdit*>("materialsNameFilter");
        auto* grid = docked->findChild<QListView*>("materialsGrid");
        REQUIRE(filter); REQUIRE(grid);
        filter->setText("target");
        app().processEvents();
        const auto index = grid->model()->index(0, 0);
        REQUIRE(index.isValid());
        CHECK(grid->model()->flags(index).testFlag(Qt::ItemIsDragEnabled));
        const QPointF at = grid->visualRect(index).center();
        auto send = [&](QEvent::Type type, Qt::MouseButtons buttons) {
            QMouseEvent event(type, at, grid->viewport()->mapToGlobal(at), Qt::LeftButton, buttons, Qt::NoModifier);
            QApplication::sendEvent(grid->viewport(), &event);
        };
        // Qt's real sequence; the first click changes the active material.
        send(QEvent::MouseButtonPress, Qt::LeftButton);
        send(QEvent::MouseButtonRelease, Qt::NoButton);
        CHECK(window.editorSession().newSolidStyle().material == "target");
        send(QEvent::MouseButtonDblClick, Qt::LeftButton);
        send(QEvent::MouseButtonRelease, Qt::NoButton);
        CHECK(sideMaterial(*doc, side) == "target");

        // A picker left open across a document change is closed, not left observing the old one.
        window.openMaterialPicker("brick", "the material", [](QString) {});
        QPointer<QDialog> picker = window.findChild<QDialog*>("materialPicker");
        REQUIRE(picker);
        window.newFile();
        CHECK_FALSE(picker);
        window.openMaterialPicker("brick", "the material", [](QString) {}); // And one left open at shutdown.
    }
    CHECK_FALSE(docked); // Destroyed with the window, after GameContent, without touching it (ASan).
}

TEST_CASE("Active material indicator shows a thumbnail and opens the shared picker") {
    app();
    const auto oldBrowserMode = QSettings().value("ui/materialBrowserWindow");
    ui::MainWindow::setMaterialBrowserInWindow(false);
    GameTree tree;
    const auto profile = makeProfile(tree);
    ui::MainWindow window;
    window.setProfiles({profile});
    waitForIndex(window.gameContent());
    window.activateMaterial("brick");
    auto* indicator = window.findChild<QToolButton*>("activeMaterialIndicator");
    REQUIRE(indicator);
    CHECK(indicator->text() == "brick");
    CHECK(indicator->toolTip().contains("brick"));
    // The indicator asks for the thumbnail itself and repaints when GameContent reports it.
    const auto swatch = [indicator] { return indicator->icon().pixmap(22, 22).toImage().pixelColor(10, 10); };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (swatch() == QColor(96, 99, 104) && std::chrono::steady_clock::now() < deadline) {
        app().processEvents();
        QThread::msleep(1);
    }
    CHECK(window.gameContent().thumbnails()->state("brick", 22) == assets::ThumbnailState::Ready);
    CHECK(swatch() != QColor(96, 99, 104));

    indicator->click();
    app().processEvents();
    auto* dialog = window.findChild<QDialog*>("materialPicker");
    REQUIRE(dialog);
    auto* picker = dialog->findChild<ui::MaterialsPanel*>("materialsPanel");
    REQUIRE(picker);
    CHECK(picker->findChild<QLineEdit*>("materialsNameFilter")->text() == "brick");
    picker->materialChosen("target");
    app().processEvents();
    CHECK(window.editorSession().newSolidStyle().material == "target");
    CHECK(indicator->text() == "target");
    if (oldBrowserMode.isValid()) QSettings().setValue("ui/materialBrowserWindow", oldBrowserMode);
    else QSettings().remove("ui/materialBrowserWindow");
}

TEST_CASE("Materials source filter lists the game and each addon that serves materials") {
    app();
    GameTree tree;
    auto profile = makeProfile(tree);
    constexpr auto header = "<!-- kv3 encoding:text:version{apex} format:generic:version{fixture} -->\n";
    tree.put("Workshop/addon.kv3", std::string(header) + "{ mod = \"Fixture Pack\" }");
    tree.put("Workshop/materials/pack/panel.vmt", "LightmappedGeneric { $basetexture white }");
    tree.put("Empty/addon.kv3", std::string(header) + "{ mod = \"Empty Pack\" }");
    tree.put("Game/addons/addonlist.kv3", std::string(header) + "{ addons = [ { dir = " +
        json::write({(tree.root / "Workshop").string()}) + " mounted = true type = \"local\" vpks = [] }, { dir = " +
        json::write({(tree.root / "Empty").string()}) + " mounted = true type = \"local\" vpks = [] } ] }");
    ui::MainWindow window;
    window.setProfiles({profile});
    waitForIndex(window.gameContent());
    ui::MaterialsPanel panel;
    panel.setGameContent(&window.gameContent());
    auto* source = panel.findChild<QComboBox*>("materialsSourceFilter");
    REQUIRE(source);
    QStringList labels;
    for (int i = 0; i < source->count(); ++i) labels << source->itemText(i);
    CHECK(labels == QStringList{"Any source", "Game content (4)", "Fixture Pack (1)"}); // Empty Pack serves none.
    CHECK(panel.filteredNames().size() == 5);
    source->setCurrentIndex(2);
    CHECK(panel.filteredNames() == QStringList{"pack/panel"});
    source->setCurrentIndex(1);
    CHECK_FALSE(panel.filteredNames().contains("pack/panel"));
    CHECK(panel.filteredNames().size() == 4);
    window.findChild<QAction*>("assets.reload")->trigger(); // The choice survives a reload while the source still exists.
    waitForIndex(window.gameContent());
    CHECK(source->currentText() == "Game content (4)");
}

TEST_CASE("Materials grid cells fit their names at the default size and with a larger font") {
    app();
    GameTree tree;
    auto profile = makeProfile(tree);
    tree.put("Game/materials/nature/blendgrassdirt_with_a_very_long_name001a.vmt", "LightmappedGeneric { $basetexture white }");
    ui::MainWindow window;
    window.setProfiles({profile});
    waitForIndex(window.gameContent());
    ui::MaterialsPanel panel;
    panel.setGameContent(&window.gameContent());
    panel.resize(700, 500);
    panel.show();
    auto* grid = panel.findChild<QListView*>("materialsGrid");
    REQUIRE(grid);
    auto* model = grid->model();
    // Wait for the real thumbnails (white) to replace the placeholders, which had a different size.
    auto loaded = [&] {
        for (int row = 0; row < model->rowCount(); ++row) {
            const auto pixmap = model->data(model->index(row, 0), Qt::DecorationRole).value<QPixmap>();
            const auto image = pixmap.toImage();
            if (image.pixelColor(image.width() / 2, image.height() / 2) != QColor(Qt::white)) return false;
        }
        return true;
    };
    // The view measures one item for every cell (uniform sizes), so every decoration must be exactly the
    // icon size and the cell must hold it plus a line of text.
    auto fits = [&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (!loaded() && std::chrono::steady_clock::now() < deadline) {
            app().processEvents();
            QThread::msleep(1);
        }
        REQUIRE(loaded());
        app().processEvents();
        REQUIRE(model->rowCount() == 5);
        const QFontMetrics metrics(grid->font());
        CHECK(grid->gridSize().height() >= grid->iconSize().height() + metrics.lineSpacing());
        for (int row = 0; row < model->rowCount(); ++row) {
            const auto pixmap = model->data(model->index(row, 0), Qt::DecorationRole).value<QPixmap>();
            CAPTURE(row);
            CHECK(pixmap.deviceIndependentSize().toSize() == grid->iconSize());
            const auto rect = grid->visualRect(model->index(row, 0));
            CHECK(rect.height() >= grid->iconSize().height() + metrics.lineSpacing());
            CHECK(rect.height() <= grid->gridSize().height());
            CHECK(rect.width() <= grid->gridSize().width());
        }
    };
    auto* slider = panel.findChild<QSlider*>("materialsThumbnailSize");
    slider->setValue(64);
    fits();
    if (const auto* out = std::getenv("APEX_MATERIALS_GRAB")) panel.grab().save(QString::fromLocal8Bit(out));
    QFont big = panel.font();
    big.setPointSizeF(big.pointSizeF() * 2);
    panel.setFont(big);
    fits();
    slider->setValue(160);
    fits();
}
