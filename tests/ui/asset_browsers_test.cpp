// Apex-authored browser fixtures, generated only under a temporary directory.
#include "test_app.hpp"
#include "../assets/fixture.hpp"
#include "../editor/fixtures.hpp"
#include <doctest/doctest.h>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDataStream>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QDialog>
#include <QListWidget>
#include <tuple>
#include <QLineEdit>
#include <QListView>
#include <QMimeData>
#include <QPushButton>
#include <QImage>
#include <QPixmap>
#include <QScrollBar>
#include <QSettings>
#include <QStatusBar>
#include <QSignalSpy>
#include <QSlider>
#include <QSpinBox>
#include <QTest>
#include <QTreeWidget>

#include "assets/addons.hpp"
#include "core/json.hpp"
#include "document/vmf.hpp"
#include "ui/game_content.hpp"
#include "ui/main_window.hpp"
#include "ui/inspector_panel.hpp"
#include "render/model_inspector.hpp"
#include "ui/model_inspect_view.hpp"
#include "ui/models_panel.hpp"
#include "ui/prefab_dialog.hpp"
#include "editor/prefab.hpp"
#include "ui/sounds_panel.hpp"
#include "ui/particles_panel.hpp"
#include "ui/viewport_area.hpp"

using namespace apex;
namespace {
QApplication& app() {
    qputenv("PULSE_SERVER", "unix:/tmp/apex-sound-preview-no-pulse");
    return ui_test::app();
}
void word(std::string& bytes, std::size_t at, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[at + i] = static_cast<char>(value >> (8 * i));
}
std::string model(bool staticProp = true) {
    std::string bytes(408, '\0');
    bytes.replace(0, 4, "IDST"); word(bytes, 4, 48);
    bytes.replace(12, 13, "Apex-authored"); word(bytes, 76, 408);
    word(bytes, 152, staticProp ? 16 : 0); word(bytes, 220, 3); word(bytes, 224, 2); word(bytes, 232, 4);
    return bytes;
}
std::string pcf(std::string_view name) {
    std::string bytes = "<!-- dmx encoding binary 1 format pcf 2 -->\n";
    bytes += '\0';
    const auto append = [&](std::uint32_t value) { const auto at = bytes.size(); bytes.resize(at + 4); word(bytes, at, value); };
    append(1);
    bytes += "DmeParticleSystemDefinition"; bytes += '\0'; bytes += name; bytes += '\0'; bytes.append(16, '\0');
    append(0); return bytes;
}
std::string wav() {
    QByteArray bytes;
    QDataStream out(&bytes, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::LittleEndian);
    out.writeRawData("RIFF", 4); out << quint32(36 + 800);
    out.writeRawData("WAVEfmt ", 8);
    out << quint32(16) << quint16(1) << quint16(1) << quint32(8000) << quint32(8000) << quint16(1) << quint16(8);
    out.writeRawData("data", 4); out << quint32(800);
    for (int i = 0; i < 800; ++i) out << quint8(128);
    return bytes.toStdString();
}
assets::GameProfile fixture(GameTree& tree, bool mounted = true) {
    tree.put("Game/gameinfo.txt", "// Apex-authored\nGameInfo { FileSystem { SearchPaths { game |gameinfo_path|. } } }");
    tree.put("Game/models/box.mdl", model());
    tree.put("Game/models/dynamic.mdl", model(false));
    tree.put("Addon/models/addon.mdl", model());
    tree.put("Game/sound/one.wav", wav()); tree.put("Game/sound/two.wav", wav());
    tree.put("Addon/sound/addon.wav", wav());
    tree.put("Game/scripts/game_sounds_manifest.txt", "// Apex-authored\ngame_sounds_manifest { file scripts/events.txt }");
    tree.put("Game/scripts/events.txt", "// Apex-authored\nApex.Event { rndwave { wave one.wav wave two.wav } }");
    tree.put("Game/particles/particles_manifest.txt", "// Apex-authored\nparticles_manifest { file particles/game.pcf file particles/addon.pcf }");
    tree.put("Game/particles/game.pcf", pcf("ApexSmoke")); tree.put("Addon/particles/addon.pcf", pcf("AddonSmoke"));
    tree.put("Game/game.fgd", R"(// Apex-authored
@PointClass = apex_fixture [
    mesh(studio) : "Mesh"
    noise(sound) : "Noise"
    effect(particlesystem) : "Effect"
    decoy(string) : "Not an asset"
]
)");
    constexpr auto header = "<!-- kv3 encoding:text:version{apex} format:generic:version{fixture} -->\n";
    tree.put("Addon/addon.kv3", std::string(header) + "{ mod = \"Browser fixture\" }");
    tree.put("Game/addons/addonlist.kv3", std::string(header) + "{ addons = [ { dir = " +
        json::write({(tree.root / "Addon").string()}) + " mounted = " + (mounted ? "true" : "false") + " type = \"local\" vpks = [] } ] }");
    assets::GameProfile profile; profile.name = "Browser fixture"; profile.gameDirectory = tree.root / "Game";
    profile.fgds = {tree.root / "Game/game.fgd"}; return profile;
}
void finish(ui::GameContent& content) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while ((!content.snapshot() || !content.modelSnapshot() || !content.soundSnapshot() || !content.particleSnapshot() || !content.classes()) &&
           std::chrono::steady_clock::now() < deadline) { app().processEvents(); QTest::qWait(1); }
    REQUIRE(content.snapshot()); REQUIRE(content.modelSnapshot()); REQUIRE(content.soundSnapshot()); REQUIRE(content.particleSnapshot()); REQUIRE(content.classes());
}
template <typename T> T* child(QWidget& panel, const char* name) {
    auto* found = panel.findChild<T*>(QString::fromLatin1(name)); REQUIRE(found); return found;
}
}
TEST_CASE("Asset browser roles round-trip and showRole constructs each panel") {
    app();
    using viewport::AreaRole;
    ui::MainWindow window;
    auto layout = viewport::ViewportLayout::quad();
    const auto panes = layout.paneIds();
    const AreaRole roles[] = {AreaRole::Models, AreaRole::Sounds, AreaRole::Particles};
    for (std::size_t i = 0; i < 3; ++i) REQUIRE(layout.setRole(panes[i], roles[i]));
    auto encoded = json::parse(json::write(layout.toJson())); REQUIRE(encoded);
    auto restored = viewport::ViewportLayout::fromJson(*encoded); REQUIRE(restored);
    window.viewportArea()->setLayout(std::move(*restored));
    CHECK(qobject_cast<ui::ModelsPanel*>(window.viewportArea()->showRole(AreaRole::Models, false)));
    CHECK(qobject_cast<ui::SoundsPanel*>(window.viewportArea()->showRole(AreaRole::Sounds, false)));
    CHECK(qobject_cast<ui::ParticlesPanel*>(window.viewportArea()->showRole(AreaRole::Particles, false)));
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(window.viewportArea()->viewportLayout().role(panes[i]) == roles[i]);
        CHECK(window.findChild<QAction*>("actionShowArea_" + QString::fromLatin1(viewport::nameOf(roles[i]))));
    }
    auto old = viewport::ViewportLayout::fromJson(viewport::ViewportLayout::quad().toJson()); REQUIRE(old);
    for (auto role : roles) CHECK(old->paneIds(role).empty());
}
TEST_CASE("Asset browsers filter name source static props sound types and script waves") {
    app(); GameTree tree; const auto profile = fixture(tree);
    ui::GameContent content; content.reload(&profile); finish(content);
    ui::ModelsPanel models; ui::SoundsPanel sounds(nullptr, ui::SoundPreview::Output::Unavailable); ui::ParticlesPanel particles;
    for (auto* panel : {static_cast<ui::AssetBrowserPanel*>(&models), static_cast<ui::AssetBrowserPanel*>(&sounds), static_cast<ui::AssetBrowserPanel*>(&particles)}) panel->setGameContent(&content);
    CHECK(models.filteredNames().size() == 3); CHECK(sounds.filteredNames().size() == 4); CHECK(particles.filteredNames().size() == 2);
    auto* modelName = child<QLineEdit>(models, "modelsNameFilter");
    modelName->setText("*BOX.*"); CHECK(models.filteredNames() == QStringList{"models/box.mdl"}); modelName->clear();
    auto* statics = child<QCheckBox>(models, "modelsStaticFilter"); statics->setChecked(true);
    CHECK_FALSE(models.filteredNames().contains("models/dynamic.mdl")); statics->setChecked(false);
    auto* type = child<QComboBox>(sounds, "soundsTypeFilter"); type->setCurrentIndex(1);
    CHECK(sounds.filteredNames() == QStringList{"Apex.Event"});
    auto* list = child<QTreeWidget>(sounds, "soundsList"); REQUIRE(list->topLevelItemCount() == 1);
    REQUIRE(list->topLevelItem(0)->childCount() == 2);
    CHECK(list->topLevelItem(0)->child(0)->text(0) == "sound/one.wav");
    CHECK(list->topLevelItem(0)->child(1)->text(0) == "sound/two.wav");
    list->setCurrentItem(list->topLevelItem(0));
    CHECK_FALSE(child<QPushButton>(sounds, "soundsPlay")->isEnabled());
    CHECK_FALSE(child<QPushButton>(sounds, "soundsNextWave")->isEnabled());
    CHECK(child<QLabel>(sounds, "soundsStatus")->text().contains("unavailable"));
    type->setCurrentIndex(2); CHECK(sounds.filteredNames().size() == 3); type->setCurrentIndex(0);
    auto* soundName = child<QLineEdit>(sounds, "soundsNameFilter");
    soundName->setText("ONE"); CHECK(sounds.filteredNames() == QStringList{"sound/one.wav"}); soundName->clear();
    auto* particleName = child<QLineEdit>(particles, "particlesNameFilter");
    particleName->setText("Apex*"); CHECK(particles.filteredNames() == QStringList{"ApexSmoke"}); particleName->clear();
    for (auto [panel, prefix] : {std::pair<ui::AssetBrowserPanel*, QString>{&models, "models"}, {&sounds, "sounds"}, {&particles, "particles"}}) {
        panel->filteredNames();
        auto* source = panel->findChild<QComboBox*>(prefix + "SourceFilter"); REQUIRE(source); REQUIRE(source->count() == 3);
        source->setCurrentIndex(source->findData(0)); CHECK(panel->filteredNames().size() == 1);
        source->setCurrentIndex(source->findData(-1)); CHECK_FALSE(panel->filteredNames().empty());
    }
}
TEST_CASE("Asset browsers used in map follows FGD studio sound particlesystem and document edits") {
    app(); GameTree tree; const auto profile = fixture(tree);
    ui::GameContent content; content.reload(&profile); finish(content);
    auto parsed = vmf::parse(R"(world { id 1 classname worldspawn }
        entity { id 2 classname apex_fixture mesh models/box.mdl noise Apex.Event effect ApexSmoke decoy AddonSmoke }
        entity { id 3 classname unknown model models/dynamic.mdl message one.wav soundname two.wav })"); REQUIRE(parsed);
    REQUIRE(content.classes()->find("apex_fixture"));
    document::Document doc(*parsed);
    ui::ModelsPanel models; ui::SoundsPanel sounds(nullptr, ui::SoundPreview::Output::Unavailable); ui::ParticlesPanel particles;
    for (auto* panel : {static_cast<ui::AssetBrowserPanel*>(&models), static_cast<ui::AssetBrowserPanel*>(&sounds), static_cast<ui::AssetBrowserPanel*>(&particles)}) {
        panel->setGameContent(&content); panel->setDocument(&doc);
        panel->findChild<QCheckBox*>(panel->objectName().chopped(5) + "UsedFilter")->setChecked(true);
    }
    CHECK(models.filteredNames() == QStringList{"models/box.mdl", "models/dynamic.mdl"});
    CHECK(sounds.filteredNames() == QStringList{"Apex.Event", "sound/one.wav", "sound/two.wav"});
    CHECK(particles.filteredNames() == QStringList{"ApexSmoke"});
    auto tx = doc.begin("Change used assets");
    tx.setKeyValue(*doc.handle(2), "mesh", "models/addon.mdl");
    tx.setKeyValue(*doc.handle(2), "noise", "addon.wav");
    tx.setKeyValue(*doc.handle(2), "effect", "AddonSmoke"); tx.commit();
    CHECK(models.filteredNames() == QStringList{"models/addon.mdl", "models/dynamic.mdl"});
    CHECK(sounds.filteredNames() == QStringList{"sound/addon.wav", "sound/one.wav", "sound/two.wav"});
    CHECK(particles.filteredNames() == QStringList{"AddonSmoke"});
    doc.undo(); CHECK(particles.filteredNames() == QStringList{"ApexSmoke"});
}
TEST_CASE("GameContent addon mounted flip and reload rebuilds every browser index") {
    app(); GameTree tree; const auto profile = fixture(tree, false);
    ui::GameContent content; content.reload(&profile);
    content.reload(&profile); // Replacement during the first workers' walk is safe.
    finish(content);
    QSignalSpy materialReady(&content, &ui::GameContent::indexReady);
    QSignalSpy modelReady(&content, &ui::GameContent::modelIndexReady);
    QSignalSpy soundReady(&content, &ui::GameContent::soundIndexReady);
    QSignalSpy particleReady(&content, &ui::GameContent::particleIndexReady);
    for (const bool mounted : {true, false}) {
        auto addons = assets::readAddonList(profile); REQUIRE(addons);
        REQUIRE(assets::setAddonMounted(*addons, 0, mounted));
        content.reload(&profile); finish(content);
        CHECK(bool(content.modelSnapshot()->find("models/addon.mdl")) == mounted);
        CHECK(bool(content.soundSnapshot()->find("sound/addon.wav")) == mounted);
        CHECK(bool(content.particleSnapshot()->find("AddonSmoke")) == mounted);
    }
    CHECK(materialReady.count() == 2); CHECK(modelReady.count() == 2); CHECK(soundReady.count() == 2); CHECK(particleReady.count() == 2);
}
TEST_CASE("Models browser requests only visible thumbnails yields to interaction and drops on hide") {
    app(); GameTree tree; const auto profile = fixture(tree);
    for (int i = 0; i < 150; ++i) tree.put("Game/models/scroll_" + std::to_string(i) + ".mdl", model());
    ui::GameContent content; content.reload(&profile); finish(content);
    std::set<QString> pending; std::size_t calls = 0;
    ui::ModelsPanel panel; panel.setGameContent(&content);
    panel.setThumbnailSink([&](const QString& path, bool visible) { if (visible) { pending.insert(path); ++calls; } else pending.erase(path); });
    panel.resize(550, 470); panel.syncThumbnails(false); CHECK(calls == 0);
    panel.show(); app().processEvents(); panel.syncThumbnails(true); CHECK(calls == 0);
    panel.syncThumbnails(false); CHECK(calls > 0); CHECK(pending.size() < 153);
    auto* grid = child<QListView>(panel, "modelsGrid");
    const auto checkVisible = [&] {
        for (const auto& path : pending) {
            const auto matches = grid->model()->match(grid->model()->index(0, 0), Qt::DisplayRole, path, 1, Qt::MatchExactly);
            REQUIRE(matches.size() == 1); CHECK(grid->visualRect(matches.front()).intersects(grid->viewport()->rect()));
        }
    };
    checkVisible(); const auto first = pending;
    grid->verticalScrollBar()->setValue(grid->verticalScrollBar()->maximum()); app().processEvents(); panel.syncThumbnails(false);
    CHECK_FALSE(pending.empty()); CHECK(pending != first); checkVisible();
    const auto before = calls; panel.syncThumbnails(true); CHECK(calls == before);
    panel.hide(); CHECK(pending.empty()); panel.syncThumbnails(false); CHECK(calls == before);
    // Model access for sizing/decoration does not schedule hidden work.
    grid->model()->data(grid->model()->index(0, 0), Qt::DecorationRole); CHECK(calls == before);
    panel.show(); app().processEvents();
    child<QComboBox>(panel, "modelsViewMode")->setCurrentIndex(1); app().processEvents();
    panel.syncThumbnails(false); CHECK_FALSE(pending.empty()); checkVisible();
    grid->verticalScrollBar()->setValue(grid->verticalScrollBar()->maximum()); app().processEvents();
    panel.syncThumbnails(false); CHECK_FALSE(pending.empty()); checkVisible();
    QSignalSpy activated(&panel, &ui::ModelsPanel::modelActivated);
    auto index = grid->model()->index(0, 0); grid->clicked(index);
    CHECK(activated.count() == 1); CHECK(grid->model()->flags(index).testFlag(Qt::ItemIsDragEnabled));
    std::unique_ptr<QMimeData> mime(grid->model()->mimeData({index}));
    REQUIRE(mime); CHECK(mime->data("application/x-apex-model") == index.data().toString().toUtf8());
    CHECK(QSettings().value("models/recent").toStringList().contains(index.data().toString()));
    panel.hide(); panel.setThumbnailSink({});
    MESSAGE("Models browser: fixture entries=153; visible requests before scroll=", first.size(), "; hidden/interaction requests=0");
}
TEST_CASE("Models browser inspect area follows selection, view and choices, and releases on hide and toggle") {
    app(); GameTree tree; const auto profile = fixture(tree);
    ui::GameContent content; content.reload(&profile); finish(content);
    QSettings().remove("models/inspect");
    ui::ModelsPanel panel; panel.setGameContent(&content);
    render::ModelInspection inspection;
    inspection.state = render::ModelInspection::State::Ready;
    inspection.image = assets::Thumbnail{2, 2, std::vector<std::uint8_t>(16, 255)};
    inspection.stats.triangles = 12; inspection.stats.vertices = 24; inspection.stats.skinFamilies = 3;
    inspection.stats.bodyParts.push_back({"head", 1, {"one", "two", "three"}});
    inspection.stats.bodyParts.push_back({"arm", 3, {"left", "right"}});
    inspection.stats.materials.push_back({"props/crate", true, assets::MaterialState::Ready, 12});
    inspection.revision = 1;
    std::vector<std::tuple<QString, render::ModelView, bool>> calls;
    panel.setInspectSink([&](const QString& path, const render::ModelView& view, bool interacting) -> const render::ModelInspection* {
        calls.emplace_back(path, view, interacting);
        return path.isEmpty() ? nullptr : &inspection;
    });
    panel.resize(550, 600); panel.show(); app().processEvents();
    auto* inspect = child<ui::ModelInspectView>(panel, "modelInspector");
    auto* toggle = child<QCheckBox>(panel, "modelsInspectToggle");
    CHECK(toggle->isChecked()); CHECK(inspect->isVisible()); CHECK_FALSE(inspect->hasModel());
    // Nothing selected: the sink is not asked.
    panel.syncThumbnails(false); CHECK(calls.empty());
    auto* grid = child<QListView>(panel, "modelsGrid");
    const auto select = [&](const QString& name) {
        const auto matches = grid->model()->match(grid->model()->index(0, 0), Qt::DisplayRole, name, 1, Qt::MatchExactly);
        REQUIRE(matches.size() == 1); grid->setCurrentIndex(matches.front());
    };
    select("models/box.mdl");
    CHECK(inspect->hasModel()); CHECK(panel.inspectedModel() == "models/box.mdl");
    panel.syncThumbnails(false);
    REQUIRE(calls.size() == 1);
    CHECK(std::get<0>(calls.back()) == "models/box.mdl"); CHECK_FALSE(std::get<2>(calls.back()));
    CHECK(std::get<1>(calls.back()) == render::ModelView{});
    CHECK(inspect->shownRevision() == 1);
    // Interaction is passed through so the host can hold renders.
    panel.syncThumbnails(true); CHECK(std::get<2>(calls.back()));
    // Skin and body choices feed the next request: body = sum(choice * base).
    auto* skin = child<QSpinBox>(*inspect, "modelInspectSkin");
    REQUIRE(skin->isVisible()); CHECK(skin->maximum() == 2);
    skin->setValue(2);
    auto* head = child<QComboBox>(*inspect, "modelInspectBody0");
    auto* arm = child<QComboBox>(*inspect, "modelInspectBody1");
    CHECK(head->count() == 3); CHECK(arm->count() == 2);
    head->setCurrentIndex(2); arm->setCurrentIndex(1);
    CHECK(inspect->view().skin == 2); CHECK(inspect->view().body == 2 + 1 * 3);
    CHECK(child<QListWidget>(*inspect, "modelInspectMaterials")->count() == 1);
    panel.syncThumbnails(false);
    CHECK(std::get<1>(calls.back()).skin == 2); CHECK(std::get<1>(calls.back()).body == 5);
    // Another model resets the choices but the sink keeps one owner.
    select("models/dynamic.mdl");
    CHECK(inspect->view().skin == 0); CHECK(inspect->view().body == 0);
    panel.syncThumbnails(false);
    CHECK(std::get<0>(calls.back()) == "models/dynamic.mdl");
    // Hiding the toggle or the panel releases the host's inspector with an empty path.
    calls.clear();
    toggle->setChecked(false); app().processEvents();
    REQUIRE(calls.size() == 1); CHECK(std::get<0>(calls.back()).isEmpty());
    CHECK_FALSE(inspect->isVisible()); CHECK_FALSE(QSettings().value("models/inspect").toBool());
    panel.syncThumbnails(false); CHECK(calls.size() == 1);
    toggle->setChecked(true); app().processEvents(); panel.syncThumbnails(false);
    CHECK(std::get<0>(calls.back()) == "models/dynamic.mdl");
    calls.clear(); panel.hide();
    REQUIRE(calls.size() == 1); CHECK(std::get<0>(calls.back()).isEmpty());
    panel.show(); app().processEvents(); calls.clear();
    // Without a sink (no GPU) the area says so and still lists the index details.
    panel.setInspectSink({}); panel.syncThumbnails(false);
    CHECK(child<QLabel>(*inspect, "modelInspectInfo")->text().contains("models/dynamic.mdl"));
    QSettings().remove("models/inspect");
}
TEST_CASE("Models inspect canvas drag orbits, pans and zooms without disturbing choices") {
    app();
    ui::ModelInspectView inspect; inspect.resize(500, 300); inspect.show(); app().processEvents();
    ui::ModelInspectView::Summary summary; summary.name = "models/box.mdl"; inspect.setModel(summary);
    render::ModelInspection inspection;
    inspection.state = render::ModelInspection::State::Ready;
    inspection.image = assets::Thumbnail{2, 2, std::vector<std::uint8_t>(16, 255)};
    inspection.stats.skinFamilies = 2;
    inspection.stats.bodyParts.push_back({"fixed", 1, {"only"}}); // Nothing to choose: no combo, but it must not crash a refresh.
    inspection.stats.bodyParts.push_back({"head", 1, {"one", "two"}});
    inspection.revision = 1;
    inspect.setInspection(&inspection);
    CHECK(inspect.findChild<QComboBox*>("modelInspectBody0") == nullptr);
    CHECK(inspect.findChild<QComboBox*>("modelInspectBody1") != nullptr);
    QSignalSpy changed(&inspect, &ui::ModelInspectView::viewChanged);
    auto* canvas = child<QWidget>(inspect, "modelInspectCanvas");
    const auto before = inspect.view();
    QTest::mousePress(canvas, Qt::LeftButton, {}, {100, 100});
    for (int i = 1; i <= 40; ++i) {
        QTest::mouseMove(canvas, {100 + i * 7, 100 - i * 3});
        if (i % 8 == 0) { ++inspection.revision; inspect.setInspection(&inspection); } // A render lands mid-drag.
    }
    QTest::mouseRelease(canvas, Qt::LeftButton, {}, {380, -20});
    CHECK(changed.count() > 0);
    CHECK(inspect.view().yaw != before.yaw); CHECK(inspect.view().pitch >= -89.0); CHECK(inspect.view().pitch <= 89.0);
    CHECK(inspect.view().yaw >= 0.0); CHECK(inspect.view().yaw < 360.0);
    // Far drags and the other buttons.
    QTest::mousePress(canvas, Qt::RightButton, {}, {10, 10});
    QTest::mouseMove(canvas, {400, 290}); QTest::mouseMove(canvas, {-600, -600});
    QTest::mouseRelease(canvas, Qt::RightButton, {}, {-600, -600});
    CHECK(std::isfinite(inspect.view().pan.x)); CHECK(inspect.view().pan != before.pan);
    QWheelEvent wheel({50, 50}, canvas->mapToGlobal(QPoint(50, 50)), {}, {0, 120}, Qt::NoButton, {}, Qt::NoScrollPhase, false);
    QApplication::sendEvent(canvas, &wheel);
    CHECK(inspect.view().zoom > 1.0);
    QTest::mouseDClick(canvas, Qt::LeftButton);
    CHECK(inspect.view().zoom == 1.0); CHECK(inspect.view().yaw == before.yaw); CHECK(inspect.view().pan == before.pan);
}
TEST_CASE("Sounds browser Play cycles script waves Next wraps and hide stops playback") {
    app(); GameTree tree; const auto profile = fixture(tree);
    ui::GameContent content; content.reload(&profile); finish(content);
    ui::SoundsPanel panel(nullptr, ui::SoundPreview::Output::DecodeOnly); panel.setGameContent(&content);
    REQUIRE(panel.preview().available()); panel.resize(600, 450); panel.show(); app().processEvents();
    auto* treeWidget = child<QTreeWidget>(panel, "soundsList");
    auto found = treeWidget->findItems("Apex.Event", Qt::MatchExactly); REQUIRE(found.size() == 1);
    treeWidget->setCurrentItem(found.front()); auto* play = child<QPushButton>(panel, "soundsPlay");
    REQUIRE(play->isEnabled()); play->click(); CHECK(panel.preview().currentPath() == "sound/one.wav");
    play->click(); CHECK(panel.preview().currentPath() == "sound/two.wav");
    auto* next = child<QPushButton>(panel, "soundsNextWave"); REQUIRE(next->isEnabled());
    next->click(); CHECK(panel.preview().currentPath() == "sound/one.wav");
    panel.hide(); CHECK_FALSE(panel.preview().isPlaying()); CHECK(panel.preview().currentPath().isEmpty());
}

TEST_CASE("Models browser reads kind-aware CPU thumbnails without a GPU") {
    app(); GameTree tree; const auto profile = fixture(tree);
    ui::GameContent content; content.reload(&profile); finish(content);
    ui::ModelsPanel panel; panel.setGameContent(&content);
    child<QLineEdit>(panel, "modelsNameFilter")->setText("box");
    REQUIRE(panel.filteredNames() == QStringList{"models/box.mdl"});
    auto* grid = child<QListView>(panel, "modelsGrid");
    const auto index = grid->model()->index(0, 0);
    const auto centerColor = [&] {
        const auto pixmap = index.data(Qt::DecorationRole).value<QPixmap>();
        return pixmap.toImage().pixelColor(pixmap.width() / 2, pixmap.height() / 2);
    };
    CHECK(centerColor() == QColor(96, 99, 104));
    assets::Thumbnail image; image.width = image.height = 128;
    image.rgba.resize(128 * 128 * 4, 255);
    for (std::size_t i = 0; i < image.rgba.size(); i += 4) { image.rgba[i] = 20; image.rgba[i + 1] = 40; image.rgba[i + 2] = 60; }
    content.thumbnails()->insert(assets::ThumbnailKind::Model, "models/box.mdl", 128, std::move(image));
    CHECK(centerColor() == QColor(20, 40, 60));
    CHECK(content.thumbnails()->residentBytes() == 65536);
    CHECK(content.thumbnails()->state(assets::ThumbnailKind::Material, "models/box.mdl", 128) == assets::ThumbnailState::Unloaded);
    MESSAGE("Model thumbnail CPU image: 128 x 128 RGBA = ", content.thumbnails()->residentBytes(), " bytes; shared cache budget=64 MiB");
}
TEST_CASE("Asset browsers pick preselect choose by double-click Enter and button and cancel with Esc") {
    app(); GameTree tree; const auto profile = fixture(tree);
    ui::GameContent content; content.reload(&profile); finish(content);
    ui::ModelsPanel models; ui::SoundsPanel sounds(nullptr, ui::SoundPreview::Output::DecodeOnly); ui::ParticlesPanel particles;
    QSignalSpy modelChosen(&models, &ui::ModelsPanel::modelChosen);
    QSignalSpy soundChosen(&sounds, &ui::SoundsPanel::soundChosen);
    QSignalSpy particleChosen(&particles, &ui::ParticlesPanel::particleChosen);
    QSignalSpy activated(&models, &ui::ModelsPanel::modelActivated);
    struct Pick { ui::AssetBrowserPanel* panel; const char* prefix; const char* initial; const char* indexed; const char* value; QSignalSpy* chosen; };
    const Pick picks[] = {
        {&models, "models", "models/box.mdl", "models/box.mdl", "models/box.mdl", &modelChosen},
        {&sounds, "sounds", "Apex.Event", "Apex.Event", "Apex.Event", &soundChosen},
        {&sounds, "sounds", "one.wav", "sound/one.wav", "one.wav", &soundChosen},
        {&sounds, "sounds", "sound/two.wav", "sound/two.wav", "two.wav", &soundChosen},
        {&particles, "particles", "ApexSmoke", "ApexSmoke", "ApexSmoke", &particleChosen}
    };
    for (const auto& pick : picks) {
        CAPTURE(pick.prefix); CAPTURE(pick.initial);
        auto& panel = *pick.panel;
        const auto prefix = QString::fromLatin1(pick.prefix);
        panel.setGameContent(&content);
        auto* filter = panel.findChild<QLineEdit*>(prefix + "NameFilter"); REQUIRE(filter);
        filter->setText("previous filter");
        panel.resize(720, 480); panel.show();
        panel.setPickMode(true, pick.initial, "the selected key"); app().processEvents();
        auto* banner = panel.findChild<QLabel*>(prefix + "PickBanner"); REQUIRE(banner);
        CHECK(banner->isVisible()); CHECK(banner->text().contains("the selected key"));
        CHECK(panel.findChild<QListWidget*>(prefix + "Recent")->isVisible());
        auto* view = prefix == "models" ? static_cast<QAbstractItemView*>(child<QListView>(panel, "modelsGrid")) :
            static_cast<QAbstractItemView*>(panel.findChild<QTreeWidget*>(prefix + "List"));
        REQUIRE(view); REQUIRE(view->currentIndex().isValid());
        CHECK(view->currentIndex().data().toString() == pick.indexed);
        pick.chosen->clear();
        QSignalSpy cancelled(&panel, &ui::AssetBrowserPanel::pickCancelled);
        const auto position = view->visualRect(view->currentIndex()).center();
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, position);
        CHECK(activated.count() == 0); CHECK(pick.chosen->count() == 0);
        QTest::mouseDClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, position);
        REQUIRE(pick.chosen->count() == 1); CHECK(pick.chosen->at(0).at(0).toString() == pick.value);
        if (prefix == "sounds") {
            CHECK(sounds.preview().currentPath().isEmpty());
            child<QPushButton>(sounds, "soundsPlay")->click();
            CHECK_FALSE(sounds.preview().currentPath().isEmpty()); CHECK(pick.chosen->count() == 1);
        }
        QTest::keyClick(view, Qt::Key_Return);
        REQUIRE(pick.chosen->count() == 2); CHECK(pick.chosen->at(1).at(0).toString() == pick.value);
        panel.findChild<QPushButton*>(prefix + "Choose")->click();
        REQUIRE(pick.chosen->count() == 3); CHECK(pick.chosen->at(2).at(0).toString() == pick.value);
        QTest::keyClick(filter, Qt::Key_Escape);
        CHECK(cancelled.count() == 1); CHECK(pick.chosen->count() == 3);
        panel.setPickMode(false);
        CHECK(filter->text() == "previous filter"); CHECK_FALSE(banner->isVisible());
        panel.hide();
    }
    models.setPickMode(true, "models/box.mdl", "a recent model"); models.show(); app().processEvents();
    auto* recent = child<QListWidget>(models, "modelsRecent");
    recent->addItem("models/dynamic.mdl"); recent->itemClicked(recent->item(recent->count() - 1));
    CHECK(child<QListView>(models, "modelsGrid")->currentIndex().data().toString() == "models/dynamic.mdl");
    CHECK(activated.count() == 0);
    modelChosen.clear(); child<QPushButton>(models, "modelsChoose")->click();
    REQUIRE(modelChosen.count() == 1); CHECK(modelChosen.at(0).at(0).toString() == "models/dynamic.mdl");
    sounds.setPickMode(true, "Apex.Event", "a script wave"); sounds.show(); app().processEvents();
    auto* list = child<QTreeWidget>(sounds, "soundsList");
    REQUIRE(list->currentItem()); REQUIRE(list->currentItem()->childCount() == 2);
    list->setCurrentItem(list->currentItem()->child(0));
    soundChosen.clear(); child<QPushButton>(sounds, "soundsChoose")->click();
    REQUIRE(soundChosen.count() == 1); CHECK(soundChosen.at(0).at(0).toString() == "one.wav");
}

TEST_CASE("Asset picker shell uses dialogs areas and one picker across all kinds and materials") {
    app(); GameTree tree; const auto profile = fixture(tree);
    const auto oldMode = QSettings().value("ui/materialBrowserWindow");
    ui::MainWindow::setMaterialBrowserInWindow(false);
    {
        ui::MainWindow window; window.setProfiles({profile}); finish(window.gameContent()); window.newFile();
        auto* area = window.viewportArea(); area->setLayout(viewport::ViewportLayout::quad());
        window.show();
        int calls = 0; QString chosen;
        const auto choose = [&](QString value) { ++calls; chosen = value; };
        window.openAssetPicker("model", "models/box.mdl", "mesh", choose); app().processEvents();
        QPointer<QDialog> dialog = window.findChild<QDialog*>("assetPicker"); REQUIRE(dialog);
        CHECK(dialog->windowFlags().testFlag(Qt::Tool)); CHECK(dialog->windowTitle() == "Choose Model");
        auto* panel = dialog->findChild<ui::ModelsPanel*>(); REQUIRE(panel);
        CHECK(child<QLabel>(*panel, "modelsPickBanner")->isVisible());
        auto* grid = child<QListView>(*panel, "modelsGrid");
        CHECK(grid->currentIndex().data().toString() == "models/box.mdl");
        child<QPushButton>(*panel, "modelsChoose")->click();
        CHECK(calls == 1); CHECK(chosen == "models/box.mdl"); CHECK_FALSE(dialog->isVisible());
        child<QPushButton>(*panel, "modelsChoose")->click(); CHECK(calls == 1);
        app().sendPostedEvents(nullptr, QEvent::DeferredDelete); CHECK(dialog.isNull());
        window.openAssetPicker("model", {}, "mesh", choose);
        dialog = window.findChild<QDialog*>("assetPicker"); REQUIRE(dialog);
        window.openMaterialPicker({}, "material", choose); CHECK(dialog.isNull());
        QPointer<QDialog> material = window.findChild<QDialog*>("materialPicker"); REQUIRE(material);
        window.openAssetPicker("sound", "Apex.Event", "noise", choose); CHECK(material.isNull());
        dialog = window.findChild<QDialog*>("assetPicker"); REQUIRE(dialog); CHECK(dialog->windowTitle() == "Choose Sound");
        window.newFile(); CHECK(dialog.isNull()); CHECK(calls == 1);
        const auto pane = area->viewportLayout().paneIds().front();
        for (auto [role, kind, prefix, initial] : {
                std::tuple{viewport::AreaRole::Models, "model", "models", "models/box.mdl"},
                std::tuple{viewport::AreaRole::Sounds, "sound", "sounds", "Apex.Event"},
                std::tuple{viewport::AreaRole::Particles, "particle", "particles", "ApexSmoke"}}) {
            area->setRole(pane, role); area->flush();
            auto* browser = qobject_cast<ui::AssetBrowserPanel*>(area->panel(pane)); REQUIRE(browser);
            auto* filter = browser->findChild<QLineEdit*>(QString(prefix) + "NameFilter"); REQUIRE(filter);
            filter->setText("before pick"); area->setCollapsed(pane, true); area->flush();
            window.openAssetPicker(kind, initial, "area pick", choose); app().processEvents();
            CHECK_FALSE(area->viewportLayout().collapsed(pane)); CHECK_FALSE(window.findChild<QDialog*>("assetPicker"));
            auto* banner = browser->findChild<QLabel*>(QString(prefix) + "PickBanner"); REQUIRE(banner); CHECK(banner->isVisible());
            const auto before = calls;
            browser->findChild<QPushButton*>(QString(prefix) + "Choose")->click();
            CHECK(calls == before + 1); CHECK(chosen == initial); CHECK_FALSE(banner->isVisible());
            CHECK(filter->text() == "before pick");
            browser->findChild<QPushButton*>(QString(prefix) + "Choose")->click(); CHECK(calls == before + 1);
            window.openAssetPicker(kind, initial, "cancel", choose);
            QTest::keyClick(filter, Qt::Key_Escape); CHECK_FALSE(banner->isVisible()); CHECK(calls == before + 1);
            window.openAssetPicker(kind, initial, "replace", choose);
            window.openMaterialPicker({}, "material", choose); CHECK_FALSE(banner->isVisible());
            window.closeMaterialPicker();
            ui::MainWindow::setMaterialBrowserInWindow(true);
            window.openAssetPicker(kind, initial, "window pick", choose);
            dialog = window.findChild<QDialog*>("assetPicker"); REQUIRE(dialog);
            CHECK(dialog->findChild<ui::AssetBrowserPanel*>() != browser);
            if (role == viewport::AreaRole::Particles) CHECK(dialog->windowTitle() == "Choose Particle System");
            window.closeMaterialPicker(); CHECK(dialog.isNull());
            ui::MainWindow::setMaterialBrowserInWindow(false);
        }
    }
    if (oldMode.isValid()) QSettings().setValue("ui/materialBrowserWindow", oldMode);
    else QSettings().remove("ui/materialBrowserWindow");
}

TEST_CASE("Inspector asset Browse opens pickers from both area and floating inspectors") {
    app(); GameTree tree; const auto profile = fixture(tree);
    const auto oldMode = QSettings().value("ui/materialBrowserWindow");
    ui::MainWindow::setMaterialBrowserInWindow(true);
    {
        ui::MainWindow window; window.setProfiles({profile}); finish(window.gameContent()); window.newFile();
        auto* doc = window.document();
        auto tx = doc->begin("Add picker target");
        auto entity = kv1::Node::block("entity");
        entity.append(kv1::Node::value("classname", "apex_fixture")).value();
        const auto handle = tx.create(std::move(entity)); tx.commit();
        const auto id = doc->get(handle)->id; window.editorSession().selectNode(id);
        auto* area = window.viewportArea();
        area->setLayout(viewport::ViewportLayout::quad());
        area->setRole(area->viewportLayout().paneIds().front(), viewport::AreaRole::Inspector); area->flush();
        for (const bool floating : {false, true}) {
            auto* inspector = qobject_cast<ui::InspectorPanel*>(area->showRole(viewport::AreaRole::Inspector, floating)); REQUIRE(inspector);
            app().processEvents();
            for (auto [key, initial, expected] : {std::tuple{"mesh", "models/box.mdl", "models/box.mdl"},
                    {"noise", "sound/one.wav", "one.wav"}, {"effect", "ApexSmoke", "ApexSmoke"}}) {
                auto* browse = inspector->findChild<QPushButton*>(QString("inspectorBrowse.") + key); REQUIRE(browse);
                REQUIRE(browse->isEnabled()); browse->click(); app().processEvents();
                auto* dialog = window.findChild<QDialog*>("assetPicker"); REQUIRE(dialog);
                auto* browser = dialog->findChild<ui::AssetBrowserPanel*>(); REQUIRE(browser);
                const auto prefix = browser->objectName().chopped(5);
                browser->findChild<QLineEdit*>(prefix + "NameFilter")->setText(initial);
                // Initial values may be empty: explicitly select the filtered row before choosing.
                auto* grid = browser->findChild<QListView*>("modelsGrid");
                auto* list = browser->findChild<QTreeWidget*>(prefix + "List");
                if (grid) grid->setCurrentIndex(grid->model()->index(0, 0));
                else { REQUIRE(list); list->setCurrentItem(list->topLevelItem(0)); }
                const auto before = doc->undoCount();
                browser->findChild<QPushButton*>(prefix + "Choose")->click();
                CHECK(doc->get(*doc->handle(id))->block->find(key)->decodedValue() == expected);
                if (!floating) CHECK(doc->undoCount() == before + 1);
                else CHECK(doc->undoCount() == before); // Picking the existing value is a no-op.
                app().sendPostedEvents(nullptr, QEvent::DeferredDelete); app().processEvents();
            }
        }
    }
    if (oldMode.isValid()) QSettings().setValue("ui/materialBrowserWindow", oldMode);
    else QSettings().remove("ui/materialBrowserWindow");
}

TEST_CASE("Models area click arms session placement and activates the Entity tool") {
    app(); GameTree tree; const auto profile = fixture(tree);
    ui::MainWindow window; window.setProfiles({profile}); finish(window.gameContent()); window.newFile();
    auto* area = window.viewportArea(); area->setLayout(viewport::ViewportLayout::quad());
    const auto pane = area->viewportLayout().paneIds().front();
    area->setRole(pane, viewport::AreaRole::Models); area->flush(); window.show(); app().processEvents();
    auto* panel = qobject_cast<ui::ModelsPanel*>(area->panel(pane)); REQUIRE(panel);
    child<QLineEdit>(*panel, "modelsNameFilter")->setText("models/box.mdl"); app().processEvents();
    auto* grid = child<QListView>(*panel, "modelsGrid");
    const auto index = grid->model()->index(0, 0); REQUIRE(index.isValid());
    QTest::mouseClick(grid->viewport(), Qt::LeftButton, Qt::NoModifier, grid->visualRect(index).center());
    auto& session = window.editorSession();
    CHECK(session.placementModel() == "models/box.mdl");
    CHECK(session.activeTool() == editor::EditorSession::Tool::Entity);
    CHECK(window.findChild<QAction*>("tool.entity")->isChecked());
    auto* picker = child<QComboBox>(window, "entityClassPicker");
    CHECK(picker->currentText() == "Model: models/box.mdl");
    CHECK(child<QLabel>(window, "statusReadout")->text() == "Entity: prop_static (box.mdl)");
    CHECK(window.statusBar()->currentMessage() == "Placing models/box.mdl");
    picker->setCurrentIndex(picker->findText("apex_fixture"));
    CHECK(session.placementModel().empty());
    CHECK(session.currentEntityClass() == "apex_fixture");
}

TEST_CASE("Prefabs tab lists the prefab folder by category, renders thumbnails, and arms a placement item") {
    app(); GameTree tree; const auto profile = fixture(tree);
    ui::MainWindow window; window.setProfiles({profile}); finish(window.gameContent()); window.newFile();
    QTemporaryDir folder; REQUIRE(folder.isValid());
    window.gameContent().setPrefabUserFolder(std::filesystem::path(folder.path().toStdString()));
    auto* doc = window.document(); REQUIRE(doc);
    const auto box = editor_test::addBox(*doc, {0, 0, 0}, {128, 64, 64});
    auto& session = window.editorSession();
    session.perform("selection.mode.solids");
    session.selectNode(editor_test::id(*doc, box));
    REQUIRE(editor::createPrefab(session, "crate", "props", std::filesystem::path(folder.path().toStdString())));
    REQUIRE(editor::createPrefab(session, "loose", "", std::filesystem::path(folder.path().toStdString())));

    auto* area = window.viewportArea(); area->setLayout(viewport::ViewportLayout::quad());
    const auto pane = area->viewportLayout().paneIds().front();
    area->setRole(pane, viewport::AreaRole::Models); area->flush(); window.show(); app().processEvents();
    auto* panel = qobject_cast<ui::ModelsPanel*>(area->panel(pane)); REQUIRE(panel);
    CHECK_FALSE(panel->prefabsShown());
    panel->showPrefabs(true); app().processEvents();
    CHECK(panel->prefabsShown());
    CHECK(panel->prefabCount() == 2);
    auto* tree_ = child<QTreeWidget>(*panel, "prefabsTree");
    REQUIRE(tree_->topLevelItemCount() == 2);
    QStringList categories;
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) categories << tree_->topLevelItem(i)->text(0);
    CHECK(categories.contains("props"));
    panel->syncThumbnails(false); app().processEvents();
    CHECK(panel->pendingPrefabIcons() == 0);
    QTreeWidgetItem* crate = nullptr;
    for (int i = 0; i < tree_->topLevelItemCount(); ++i)
        if (tree_->topLevelItem(i)->text(0) == "props") crate = tree_->topLevelItem(i)->child(0);
    REQUIRE(crate);
    CHECK(crate->text(0) == "crate");
    CHECK_FALSE(crate->icon(0).isNull());

    session.perform(editor::EditorSession::Tool::Select);
    tree_->scrollToItem(crate);
    QTest::mouseClick(tree_->viewport(), Qt::LeftButton, Qt::NoModifier, tree_->visualItemRect(crate).center());
    CHECK(session.activeTool() == editor::EditorSession::Tool::Entity);
    const auto item = session.placementItem();
    const auto* prefab = std::get_if<editor::PrefabItem>(&item);
    REQUIRE(prefab);
    CHECK(std::filesystem::path(prefab->path).filename() == "crate.vmf");
    REQUIRE(prefab->bounds);
    CHECK(prefab->bounds->min.z == doctest::Approx(0.0));
    CHECK(prefab->bounds->max.x == doctest::Approx(64.0));

    // A prefab written later shows up after a refresh.
    REQUIRE(editor::createPrefab(session, "later", "props", std::filesystem::path(folder.path().toStdString())));
    panel->refreshPrefabs();
    CHECK(panel->prefabCount() == 3);
}

TEST_CASE("Create Prefab from Selection opens its dialog, writes the file and refreshes the list") {
    app(); GameTree tree; const auto profile = fixture(tree);
    ui::MainWindow window; window.setProfiles({profile}); finish(window.gameContent()); window.newFile();
    QTemporaryDir folder; REQUIRE(folder.isValid());
    const auto root = std::filesystem::path(folder.path().toStdString());
    window.gameContent().setPrefabUserFolder(root);
    auto* action = window.findChild<QAction*>("edit.create_prefab"); REQUIRE(action);
    CHECK_FALSE(action->isEnabled());
    auto* doc = window.document(); REQUIRE(doc);
    const auto box = editor_test::addBox(*doc, {0, 0, 0}, {64, 64, 64});
    auto& session = window.editorSession();
    session.perform("selection.mode.solids");
    session.selectNode(editor_test::id(*doc, box));
    app().processEvents();
    CHECK(action->isEnabled());
    int changed = 0;
    QObject::connect(&window.gameContent(), &ui::GameContent::prefabsChanged, [&] { ++changed; });
    action->trigger();
    auto* dialog = window.findChild<ui::PrefabDialog*>("prefabDialog"); REQUIRE(dialog);
    dialog->setPrefabName("");
    CHECK_FALSE(dialog->create());
    CHECK_FALSE(dialog->error().isEmpty());
    dialog->setPrefabName("a/b");
    CHECK_FALSE(dialog->create());
    dialog->setPrefabName("wall"); dialog->setCategory("walls");
    dialog->accept();
    CHECK(std::filesystem::exists(root / "walls" / "wall.vmf"));
    CHECK(changed == 1);
    app().sendPostedEvents(nullptr, QEvent::DeferredDelete);
    CHECK(window.findChild<ui::PrefabDialog*>("prefabDialog") == nullptr);
    // The same name again is refused unless replacing is ticked.
    action->trigger();
    dialog = window.findChild<ui::PrefabDialog*>("prefabDialog"); REQUIRE(dialog);
    dialog->setPrefabName("wall"); dialog->setCategory("walls");
    CHECK_FALSE(dialog->create());
    dialog->setOverwrite(true);
    CHECK(dialog->create());
    dialog->reject();
}

TEST_CASE("Model inspector animation choices scrub frames and stop playback on hide or model changes") {
    app();
    ui::ModelInspectView inspect;
    inspect.resize(700, 500);
    inspect.show();
    ui::ModelInspectView::Summary summary;
    summary.name = "models/animated.mdl";
    inspect.setModel(summary);
    render::ModelInspection inspection;
    inspection.state = render::ModelInspection::State::Ready;
    inspection.revision = 1;
    inspection.stats.sequences = {{"idle", 30, 120, true, {}},
                                 {"open", 24, 61, true, {}},
                                 {"external", 30, 90, false, "External animation data is unavailable"}};
    inspect.setInspection(&inspection);
    auto* sequence = child<QComboBox>(inspect, "modelInspectSequence");
    auto* frame = child<QDoubleSpinBox>(inspect, "modelInspectFrame");
    auto* scrub = child<QSlider>(inspect, "modelInspectScrub");
    auto* play = child<QPushButton>(inspect, "modelInspectPlay");
    auto* speed = child<QDoubleSpinBox>(inspect, "modelInspectSpeed");
    auto* status = child<QLabel>(inspect, "modelInspectAnimationStatus");
    REQUIRE(sequence->count() == 3);
    CHECK(play->isEnabled());
    CHECK(frame->maximum() == 119);
    CHECK(scrub->maximum() == 119);
    QSignalSpy changed(&inspect, &ui::ModelInspectView::viewChanged);
    sequence->setCurrentIndex(1);
    CHECK(inspect.view().sequence == 1);
    CHECK(frame->maximum() == 60);
    scrub->setValue(23);
    CHECK(inspect.view().frame == 23);
    frame->setValue(12.5);
    CHECK(inspect.view().frame == 12.5);
    CHECK(scrub->value() == 12);
    REQUIRE(changed.count() >= 3);
    // New renders retain the requested frame and sequence while controls are refreshed.
    ++inspection.revision;
    inspect.setInspection(&inspection);
    CHECK(inspect.view().sequence == 1);
    CHECK(inspect.view().frame == 12.5);
    speed->setValue(2);
    play->click();
    REQUIRE(play->isChecked());
    QTest::qWait(80);
    CHECK(inspect.view().frame > 12.5);
    play->click();
    CHECK_FALSE(play->isChecked());
    const auto paused = inspect.view().frame;
    QTest::qWait(50);
    CHECK(inspect.view().frame == paused);
    play->click();
    inspect.hide();
    CHECK_FALSE(play->isChecked());
    const auto hidden = inspect.view().frame;
    QTest::qWait(50);
    CHECK(inspect.view().frame == hidden);
    inspect.show();
    sequence->setCurrentIndex(2);
    CHECK_FALSE(play->isEnabled());
    CHECK_FALSE(frame->isEnabled());
    CHECK(status->text().contains("External animation"));
    CHECK(inspect.view().frame == 0);
    sequence->setCurrentIndex(0);
    play->click();
    REQUIRE(play->isChecked());
    summary.name = "models/other.mdl";
    inspect.setModel(summary);
    CHECK_FALSE(play->isChecked());
    CHECK(inspect.view().sequence == 0);
    CHECK(inspect.view().frame == 0);
    CHECK(sequence->count() == 0);
    CHECK_FALSE(play->isEnabled());
    inspect.clearModel();
    CHECK_FALSE(inspect.hasModel());
}
