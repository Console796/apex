// apex.vfs/1 in the UI: a plugin provider's materials reach the browser, thumbnails and the Vfs the renderer
// reads, earlier mounts keep winning, and a compile says that provider mounts are left out.
#include "test_app.hpp"
#include "../assets/fixture.hpp"
#include "../compile/scripted_runner.hpp"
#include "../editor/fixtures.hpp"
#include "plugin_host/plugin_host.hpp"
#include "ui/compile_panel.hpp"
#include "ui/game_content.hpp"
#include "ui/materials_panel.hpp"

#include <doctest/doctest.h>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>
#include <chrono>
#include <algorithm>
#include <fstream>

using namespace apex;

namespace {
// Plugin actions register in process-wide registries, and a second Host cannot claim the same ids, so
// every case shares one plugin host for the whole run.
struct SharedHost {
    std::filesystem::path plugins = std::filesystem::temp_directory_path() /
        ("apex-ui-vfs-" + std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    std::unique_ptr<plugin_host::Host> host;
    std::vector<std::string> logs;
    SharedHost() {
        std::filesystem::create_directories(plugins / "vfs");
#ifdef _WIN32
        constexpr auto platform = "windows-x86_64";
#else
        constexpr auto platform = "linux-x86_64";
#endif
        std::ofstream(plugins / "vfs" / "plugin.kv3")
            << "<!-- kv3 encoding:text:version{apex} format:generic:version{fixture} -->\n"
            << "{ id = \"vfs\" name = \"Vfs\" version = \"1.0.0\" apex_abi = { major = 1 minor = 0 } entry = { \""
            << platform << "\" = \"" << VFS_PLUGIN_PATH << "\" } }";
        host = std::make_unique<plugin_host::Host>(plugins / "state", plugins / "breadcrumb",
                                                   [this](std::string_view line) { logs.emplace_back(line); });
        host->discover({plugins});
        host->activate();
    }
    ~SharedHost() {
        host.reset();
        std::error_code ec;
        std::filesystem::remove_all(plugins, ec);
    }
};
SharedHost& sharedHost() { static SharedHost shared; return shared; }
struct ProviderRig {
    GameTree tree;
    assets::GameProfile profile;
    std::unique_ptr<plugin_host::Host>& host = sharedHost().host;
    std::vector<std::string>& logs = sharedHost().logs;
    bool logged(const std::string& line) const { return std::ranges::find(logs, line) != logs.end(); }
    ProviderRig() {
        logs.clear();
        tree.put("Game/gameinfo.txt", "GameInfo { FileSystem { SearchPaths { game |gameinfo_path|. } } }");
        tree.put("Game/materials/shared.vmt", "UnlitGeneric { $basetexture game/shared }");
        profile.gameDirectory = tree.root / "Game";
        for (const auto* tool : {"vbsp", "vvis", "vrad", "postcompiler"}) {
            auto file = tree.root / tool;
            std::ofstream(file) << "fake";
            profile.toolPaths[tool] = file;
        }
    }
};
void waitFor(ui::GameContent& content) {
    for (int i = 0; i < 15000 && (!content.snapshot() || !content.vfs()); ++i) QTest::qWait(1);
    REQUIRE_MESSAGE(content.vfs(), content.error().toStdString());
    REQUIRE(content.snapshot());
}
}

TEST_CASE("ui: a plugin provider's materials appear in the index, the browser and the Vfs, and a compile reports the omission") {
    ui_test::app();
    ProviderRig rig;
    REQUIRE(rig.host->plugins().size() == 1);
    REQUIRE(rig.host->plugins()[0].loaded);
    ui::GameContent content;
    content.setVfsProviderSource([&] { return rig.host->vfsProviders(); });
    content.reload(&rig.profile);
    waitFor(content);
    const auto snapshot = content.snapshot();
    REQUIRE(snapshot->find("vfsplugin/glass"));
    CHECK(snapshot->find("vfsplugin/glass")->surfaceProp == "glass");
    // Earlier mounts win: the game's shared.vmt is indexed from the game, not the provider.
    REQUIRE(snapshot->find("shared"));
    CHECK(snapshot->find("shared")->previewTexture == "game/shared");
    CHECK(content.assetExists("materials/vfsplugin/glass.vmt"));
    REQUIRE(content.vfs()->open("materials/vfsplugin/glass.vmt"));
    CHECK(content.thumbnails());   // the thumbnail cache is built over the same Vfs

    ui::MaterialsPanel panel;
    panel.setGameContent(&content);
    panel.resize(800, 500);
    panel.show();
    QTest::qWait(10);
    CHECK(panel.filteredNames().contains("vfsplugin/glass"));
    CHECK(panel.filteredNames().contains("shared"));

    // A compile: the provider mount cannot go into the compile gameinfo, and the panel says so.
    document::Document doc(editor_test::parsed("world { id 1 classname worldspawn }"));
    compile::test::ScriptedRunner runner;
    runner.scripts = {{{"vbsp"}}, {{"post"}}, {{"vis"}}, {{"light"}}};
    ui::CompilePanel compile;
    compile.setGameContent(&content);
    compile.setDocument(&doc);
    compile.setCacheDirectory(rig.tree.root / "cache");
    compile.setProcessRunner(&runner);
    auto* run = compile.findChild<QPushButton*>("compileRun");
    auto* header = compile.findChild<QLabel*>("compileHeader");
    REQUIRE(run); REQUIRE(header);
    compile.refresh();
    run->click();
    compile.refresh();
    CHECK(header->text().contains("Test files"));
    CHECK(header->text().contains("left out of this compile"));
    compile.setDocument(nullptr);
}

TEST_CASE("ui: refreshMaterial re-indexes one written material and signals the panels") {
    ui_test::app();
    ProviderRig rig;
    rig.tree.put("Game/materials/edit.vmt", "UnlitGeneric { $basetexture edit/a }");
    ui::GameContent content;
    content.reload(&rig.profile);
    waitFor(content);
    REQUIRE(content.snapshot()->find("edit"));
    CHECK(content.snapshot()->find("edit")->surfaceProp.empty());
    rig.tree.put("Game/materials/edit.vmt", "UnlitGeneric { $basetexture edit/a $surfaceprop metal }");
    QSignalSpy ready(&content, &ui::GameContent::indexReady);
    CHECK(content.refreshMaterial("edit"));
    CHECK(ready.count() == 1);
    CHECK(content.snapshot()->find("edit")->surfaceProp == "metal");
    CHECK_FALSE(content.refreshMaterial("nonexistent"));
}

#include "assets/thumbnail_cache.hpp"
#include "../assets/texture_fixture.hpp"
#include "document/document.hpp"
#include "ui/main_window.hpp"
#include <QThread>

TEST_CASE("ui: a provider's texture gets a thumbnail through the shared Vfs") {
    ui_test::app();
    ProviderRig rig;
    auto texture = whiteVtf();
    REQUIRE(texture.bake(rig.tree.root / "glass.vtf"));
    qputenv("APEX_TEST_VFS_VTF", QByteArray::fromStdString((rig.tree.root / "glass.vtf").string()));
    // The provider reads the variable when it lists, so reload after setting it.
    ui::GameContent content;
    content.setVfsProviderSource([&] { return rig.host->vfsProviders(); });
    content.reload(&rig.profile);
    waitFor(content);
    qunsetenv("APEX_TEST_VFS_VTF");
    REQUIRE(content.thumbnails());
    CHECK(content.assetExists("materials/vfsplugin/glass.vtf"));
    for (int i = 0; i < 10000 && !content.thumbnails()->request("vfsplugin/glass", 64); ++i) QTest::qWait(1);
    CHECK(content.thumbnails()->state("vfsplugin/glass", 64) == assets::ThumbnailState::Ready);
}

TEST_CASE("ui: the window's apex.assets/1 backend reads provider sources and writes loose VMTs") {
    ui_test::app();
    ProviderRig rig;
    rig.tree.put("Game/materials/loose.vmt", "UnlitGeneric { $basetexture loose/a // keep\n}");
    // The document outlives the window and the session that is attached to it.
    auto document = std::make_unique<document::Document>(editor_test::parsed("world { id 1 classname worldspawn }"));
    ui::MainWindow window;
    window.setPluginHost(rig.host.get());
    window.setProfiles({rig.profile});
    waitFor(window.gameContent());
    window.editorSession().attach(*document);
    rig.host->setSession(&window.editorSession());
    rig.host->setDocument(document.get());
    qputenv("APEX_TEST_VFS_MATERIAL", "vfsplugin/glass");
    qputenv("APEX_TEST_VFS_VALUE", "metal");
    REQUIRE(window.editorSession().perform("plugin.vfs.find"));
    CHECK(rig.logged("find=0:vfsplugin/glass:vfsplugin/glass:glass:provider:Test files"));
    // A provider material cannot be written: UNSUPPORTED, and the index is untouched.
    REQUIRE(window.editorSession().perform("plugin.vfs.write"));
    CHECK(std::ranges::any_of(rig.logs, [](const auto& l) { return l.starts_with("write=" + std::to_string(APEX_ERR_UNSUPPORTED) + ":"); }));
    CHECK(window.gameContent().snapshot()->find("vfsplugin/glass")->surfaceProp == "glass");
    // A loose material is written through Apex's writer and re-indexed.
    qputenv("APEX_TEST_VFS_MATERIAL", "loose");
    REQUIRE(window.editorSession().perform("plugin.vfs.write"));
    std::ifstream in(rig.tree.root / "Game/materials/loose.vmt");
    const std::string text(std::istreambuf_iterator<char>(in), {});
    CHECK(text.find("// keep") != std::string::npos);
    CHECK(text.find("metal") != std::string::npos);
    CHECK(window.gameContent().snapshot()->find("loose")->surfaceProp == "metal");
    qputenv("APEX_TEST_VFS_MATERIAL", "loose");
    REQUIRE(window.editorSession().perform("plugin.vfs.find"));
    CHECK(rig.logged("find=0:loose:loose/a:metal:game"));
    qunsetenv("APEX_TEST_VFS_MATERIAL"); qunsetenv("APEX_TEST_VFS_VALUE");
    rig.host->setSession(nullptr);
    rig.host->setDocument(nullptr);
}

TEST_CASE("ui: reloading content while the first provider walk can still be running is safe") {
    ui_test::app();
    ProviderRig rig;
    ui::GameContent content;
    content.setVfsProviderSource([&] { return rig.host->vfsProviders(); });
    content.reload(&rig.profile);
    content.reload(&rig.profile);   // replaces the Vfs while the first load and walk may be in flight
    content.reload(&rig.profile);
    waitFor(content);
    CHECK(content.snapshot()->find("vfsplugin/glass"));
    for (int i = 0; i < 3; ++i) { content.reload(&rig.profile); QTest::qWait(2); }
    waitFor(content);
    CHECK(content.snapshot()->find("vfsplugin/glass"));
}
