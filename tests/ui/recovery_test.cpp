#include "test_app.hpp"
#include <doctest/doctest.h>

#include "ui/recovery_dialog.hpp"
#include "ui/main_window.hpp"
#include "document/document.hpp"

#include <QApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QSettings>
#include <QTimer>
#include <QAction>
#include <QCheckBox>
#include <QSpinBox>
#include <QDialogButtonBox>


TEST_CASE("recovery controller keeps later entries and refuses failed opens") {
    apex::ui_test::app();
    apex::editor::Recovery entry;
    entry.source = "original.vmf";
    entry.snapshot = "snapshot.vmf";
    bool called = false;
    apex::ui::RecoveryDialog dialog({entry}, [&](const auto& recovery) {
        called = true;
        CHECK(recovery.source == entry.source);
        return false;
    });
    CHECK_FALSE(dialog.choose(0, apex::ui::RecoveryDialog::Choice::Open));
    CHECK(called);
    CHECK(dialog.count() == 1);
    CHECK(dialog.choose(0, apex::ui::RecoveryDialog::Choice::Later));
    CHECK(dialog.count() == 0);
}

TEST_CASE("recovery opens an unsaved map at its source path and save clears unsaved state") {
    apex::ui_test::app();
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const auto previousRoot = qgetenv("APEX_AUTOSAVE_DIR");
    const auto previousSkip = qgetenv("APEX_SKIP_RECOVERY");
    qputenv("APEX_AUTOSAVE_DIR", (dir.path() + "/state").toUtf8());
    qputenv("APEX_SKIP_RECOVERY", "1");
    const auto snapshot = dir.path() + "/000-recovery.vmf";
    QFile file(snapshot);
    REQUIRE(file.open(QIODevice::WriteOnly));
    file.write("world { id 1 classname worldspawn }");
    file.close();
    {
        apex::ui::MainWindow window;
        apex::editor::Recovery entry;
        entry.source = (dir.path() + "/original.vmf").toStdString();
        entry.snapshot = snapshot.toStdString();
        entry.lock = (dir.path() + "/recovery.lock").toStdString();
        QFile lock(QString::fromStdString(entry.lock.string()));
        REQUIRE(lock.open(QIODevice::WriteOnly));
        lock.write("0\n");
        lock.close();
        apex::ui::RecoveryDialog later({entry}, [](const auto&) { return false; });
        REQUIRE(later.choose(0, apex::ui::RecoveryDialog::Choice::Later));
        CHECK(QFile::exists(snapshot));
        CHECK(QFile::exists(QString::fromStdString(entry.lock.string())));
        apex::ui::RecoveryDialog dialog({entry}, [&](const auto& candidate) {
            return window.recoverAutosave(candidate);
        });
        REQUIRE(dialog.choose(0, apex::ui::RecoveryDialog::Choice::Open));
        CHECK(dialog.count() == 0);
        CHECK_FALSE(QFile::exists(snapshot));
        CHECK_FALSE(QFile::exists(QString::fromStdString(entry.lock.string())));
        CHECK(window.document() != nullptr);
        CHECK(window.documentPath().toStdString() == entry.source.string());
        CHECK(window.windowTitle().startsWith("* "));
        REQUIRE(window.saveFile(window.documentPath()));
        CHECK_FALSE(window.windowTitle().startsWith("* "));
        CHECK(QFile::exists(window.documentPath()));
        window.newFile();
        CHECK(window.documentPath().isEmpty());
        CHECK(window.document() != nullptr);
        window.close();
        for (const auto& item : std::filesystem::recursive_directory_iterator(
                 (dir.path() + "/state").toStdString()))
            CHECK(item.path().extension() != ".lock");
    }
    if (previousRoot.isNull()) qunsetenv("APEX_AUTOSAVE_DIR"); else qputenv("APEX_AUTOSAVE_DIR", previousRoot);
    if (previousSkip.isNull()) qunsetenv("APEX_SKIP_RECOVERY"); else qputenv("APEX_SKIP_RECOVERY", previousSkip);
}

TEST_CASE("autosave settings persist enabled and interval and update the timer") {
    apex::ui_test::app();
    QSettings settings;
    const auto oldEnabled = settings.value("autosave/enabled");
    const auto oldInterval = settings.value("autosave/intervalMinutes");
    settings.remove("autosave/enabled");
    settings.remove("autosave/intervalMinutes");
    {
        apex::ui::MainWindow window;
        auto* timer = window.findChild<QTimer*>("autosaveTimer");
        REQUIRE(timer);
        CHECK(timer->isActive());
        CHECK(timer->interval() == 300000);
        auto* action = window.findChild<QAction*>("actionAutosaveSettings");
        REQUIRE(action);
        action->trigger();
        auto* dialog = window.findChild<QDialog*>("autosaveSettingsDialog");
        REQUIRE(dialog);
        auto* enabled = dialog->findChild<QCheckBox*>();
        auto* interval = dialog->findChild<QSpinBox*>();
        REQUIRE(enabled);
        REQUIRE(interval);
        enabled->setChecked(false);
        interval->setValue(2);
        dialog->findChild<QDialogButtonBox*>()->accepted();
        CHECK_FALSE(timer->isActive());
        CHECK_FALSE(settings.value("autosave/enabled").toBool());
        CHECK(settings.value("autosave/intervalMinutes").toInt() == 2);
    }
    if (oldEnabled.isValid()) settings.setValue("autosave/enabled", oldEnabled);
    else settings.remove("autosave/enabled");
    if (oldInterval.isValid()) settings.setValue("autosave/intervalMinutes", oldInterval);
    else settings.remove("autosave/intervalMinutes");
}
