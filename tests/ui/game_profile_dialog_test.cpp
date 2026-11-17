// Headless tests for Add Game: the profile dialog and saving a custom game profile.
#include "test_app.hpp"
#include <doctest/doctest.h>

#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLineEdit>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "assets/game_profile.hpp"
#include "ui/game_profile_dialog.hpp"
#include "ui/main_window.hpp"

namespace {
using namespace apex;

void put(const QString& path, const QByteArray& text) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    file.write(text);
}

QString makeGame(const QTemporaryDir& dir) {
    const auto game = dir.filePath(QStringLiteral("install/mymod"));
    put(game + QStringLiteral("/gameinfo.txt"),
        "GameInfo\n{\n game \"My Mod\"\n FileSystem\n {\n  SearchPaths\n  {\n   game |gameinfo_path|.\n  }\n }\n}\n");
    put(game + QStringLiteral("/mymod.fgd"), "");
    return game;
}
} // namespace

TEST_CASE("Add Game dialog fills from the game folder and validates") {
    ui_test::app();
    QTemporaryDir dir;
    const auto game = makeGame(dir);
    ui::GameProfileDialog dialog(nullptr, {QStringLiteral("Taken")});
    CHECK_FALSE(dialog.validate().isEmpty());
    dialog.setGameDirectory(dir.path());
    CHECK_FALSE(dialog.validate().isEmpty()); // no gameinfo.txt there
    dialog.setGameDirectory(game);
    CHECK(dialog.validate().isEmpty());
    auto profile = dialog.profile();
    CHECK(profile.name == "My Mod");
    CHECK(profile.gameDirectory == std::filesystem::path(game.toStdU16String()));
    REQUIRE(profile.fgds.size() == 1);
    dialog.findChild<QLineEdit*>(QStringLiteral("gameName"))->setText(QStringLiteral("Taken"));
    CHECK_FALSE(dialog.validate().isEmpty());

    // Editing keeps the profile's own name allowed.
    assets::GameProfile existing = profile;
    existing.name = "Taken";
    ui::GameProfileDialog edit(&existing, {QStringLiteral("Taken")});
    CHECK(edit.validate().isEmpty());
    CHECK(edit.profile() == existing);
}

TEST_CASE("Saving a custom game profile adds and selects it in the picker") {
    ui_test::app();
    QTemporaryDir dir;
    const auto game = makeGame(dir);
    const auto profiles = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/profiles");
    QDir(profiles).removeRecursively();
    auto suggested = assets::suggestGameProfile(std::filesystem::path(game.toStdU16String()));
    REQUIRE(suggested);
    ui::MainWindow window;
    CHECK(window.saveGameProfile(*suggested).isEmpty());
    CHECK(QFile::exists(profiles + QStringLiteral("/My Mod.json")));
    REQUIRE(window.currentProfile());
    CHECK(window.currentProfile()->name == "My Mod");
    CHECK(window.profilePicker()->isEnabled());
    // A second profile is selected too, and the first stays listed.
    auto second = *suggested;
    second.name = "Other";
    CHECK(window.saveGameProfile(second).isEmpty());
    CHECK(window.currentProfile()->name == "Other");
    CHECK(window.profilePicker()->findText(QStringLiteral("My Mod")) >= 0);
    QDir(profiles).removeRecursively();
}
