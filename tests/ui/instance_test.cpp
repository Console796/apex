#include "test_app.hpp"
#include <doctest/doctest.h>

#include "ui/main_window.hpp"
#include "ui/viewport_area.hpp"
#include "ui/viewport_pane.hpp"
#include "editor/group_ops.hpp"

#include <QAction>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QElapsedTimer>
#include <QSaveFile>
#include <QPushButton>
#include <QCoreApplication>

namespace {
using namespace apex;

QString parentMap(const QString& templateName, int count = 1) {
    QString text = QStringLiteral("versioninfo { \"editorversion\" \"400\" }\nworld { \"id\" \"1\" \"classname\" \"worldspawn\" }\n");
    for (int i = 0; i < count; ++i)
        text += QStringLiteral("entity { \"id\" \"%1\" \"classname\" \"func_instance\" \"targetname\" \"i%1\" \"file\" \"%2\" \"origin\" \"%3 0 0\" \"angles\" \"0 0 0\" }\n")
            .arg(i + 2).arg(templateName).arg(i * 128);
    return text;
}

bool writeFile(const QString& path, const QByteArray& bytes, bool replace = false) {
    if (!replace) {
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    }
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

bool waitFor(const std::function<bool()>& predicate, int timeout = 3000) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < timeout) {
        if (predicate()) return true;
        QTest::qWait(20);
    }
    return predicate();
}

QString relay(const QString& name) {
    return QStringLiteral("entity { \"id\" \"1\" \"classname\" \"logic_relay\" \"targetname\" \"%1\" }").arg(name);
}

}

TEST_CASE("instance menu collapse actions operate on the selected instance as one undo step") {
    ui_test::app();
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const auto templ = dir.filePath(QStringLiteral("leaf.vmf"));
    const auto map = dir.filePath(QStringLiteral("parent.vmf"));
    REQUIRE(writeFile(templ, relay("leaf").toUtf8()));
    REQUIRE(writeFile(map, parentMap("leaf.vmf", 2).toUtf8()));
    ui::MainWindow window;
    REQUIRE(window.openFile(map));
    auto& session = window.editorSession();
    REQUIRE(session.instances().contents().size() == 2);
    auto* collapse = window.findChild<QAction*>(QStringLiteral("instance.collapse"));
    auto* collapseAll = window.findChild<QAction*>(QStringLiteral("instance.collapse_all"));
    REQUIRE(collapse);
    REQUIRE(collapseAll);
    CHECK(collapse->isEnabled() == false);
    CHECK(collapseAll->isEnabled());
    const auto instance = session.instances().contents().front().instance;
    session.selection().select(instance);
    REQUIRE(waitFor([&] { return collapse->isEnabled(); }));
    const auto before = window.document()->write().value();
    collapse->trigger();
    CHECK_FALSE(window.document()->write().value() == before);
    auto* undo = window.findChild<QAction*>(QStringLiteral("edit.undo"));
    REQUIRE(undo);
    undo->trigger();
    CHECK(window.document()->write().value() == before);
    collapseAll->trigger();
    CHECK(window.document()->write().value() != before);
    CHECK_FALSE(collapseAll->isEnabled());
    undo->trigger();
    CHECK(window.document()->write().value() == before);
}

TEST_CASE("Open Instance records the executable and resolved path and disables unresolved instances") {
    ui_test::app();
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const auto leaf = dir.filePath(QStringLiteral("leaf.vmf"));
    const auto map = dir.filePath(QStringLiteral("parent.vmf"));
    REQUIRE(writeFile(leaf, relay("leaf").toUtf8()));
    REQUIRE(writeFile(map, parentMap("leaf.vmf").toUtf8()));
    ui::MainWindow window;
    REQUIRE(window.openFile(map));
    const auto node = window.editorSession().instances().contents().front().instance;
    window.editorSession().selection().select(node);
    window.showInspector();
    QCoreApplication::processEvents();
    auto* inspectorOpen = window.findChild<QPushButton*>(QStringLiteral("openInstance"));
    REQUIRE(inspectorOpen);
    auto* open = window.findChild<QAction*>(QStringLiteral("instance.open"));
    REQUIRE(open);
    REQUIRE(waitFor([&] { return open->isEnabled(); }));
    QString executable;
    QStringList arguments;
    window.setInstanceLauncher([&](const QString& exe, const QStringList& args) {
        executable = exe; arguments = args; return true;
    });
    inspectorOpen->click();
    CHECK(executable == QCoreApplication::applicationFilePath());
    REQUIRE(arguments.size() == 1);
    CHECK(QFileInfo(arguments.front()).canonicalFilePath() == QFileInfo(leaf).canonicalFilePath());

    const auto missingMap = dir.filePath(QStringLiteral("missing-parent.vmf"));
    REQUIRE(writeFile(missingMap, parentMap("absent.vmf").toUtf8()));
    ui::MainWindow unresolved;
    REQUIRE(unresolved.openFile(missingMap));
    const auto missing = unresolved.editorSession().instances().problems().front().instance;
    unresolved.editorSession().selection().select(missing);
    auto* missingOpen = unresolved.findChild<QAction*>(QStringLiteral("instance.open"));
    REQUIRE(missingOpen);
    CHECK_FALSE(missingOpen->isEnabled());
    CHECK_FALSE(missingOpen->toolTip().isEmpty());
}

TEST_CASE("instance watcher reloads changed and atomically replaced templates, preserving parent bytes and last good template") {
    ui_test::app();
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const auto leaf = dir.filePath(QStringLiteral("leaf.vmf"));
    const auto map = dir.filePath(QStringLiteral("parent.vmf"));
    REQUIRE(writeFile(leaf, relay("before").toUtf8()));
    REQUIRE(writeFile(map, parentMap("leaf.vmf").toUtf8()));
    ui::MainWindow window;
    REQUIRE(window.openFile(map));
    auto* watcher = window.findChild<QFileSystemWatcher*>(QStringLiteral("instanceWatcher"));
    REQUIRE(watcher);
    QSignalSpy fileSpy(watcher, &QFileSystemWatcher::fileChanged);
    QSignalSpy directorySpy(watcher, &QFileSystemWatcher::directoryChanged);
    REQUIRE(waitFor([&] { return watcher->files().contains(leaf); }));
    const auto parentBytes = window.document()->write().value();
    auto prior = window.editorSession().instances().contents().front().templ;
    REQUIRE(writeFile(leaf, relay("after-write").toUtf8()));
    REQUIRE(waitFor([&] { return window.editorSession().instances().contents().front().templ != prior; }));
    CHECK(window.editorSession().instances().contents().front().templ->write()->find("after-write") != std::string::npos);
    CHECK(window.document()->write().value() == parentBytes);
    CHECK((fileSpy.count() > 0 || directorySpy.count() > 0));

    prior = window.editorSession().instances().contents().front().templ;
    REQUIRE(writeFile(leaf, relay("after-rename").toUtf8(), true));
    REQUIRE(waitFor([&] { return window.editorSession().instances().contents().front().templ != prior; }));
    CHECK(window.editorSession().instances().contents().front().templ->write()->find("after-rename") != std::string::npos);
    prior = window.editorSession().instances().contents().front().templ;
    REQUIRE(writeFile(leaf, QByteArray("entity {"), true));
    REQUIRE(waitFor([&] { return !window.editorSession().instances().problems().empty(); }));
    CHECK(window.editorSession().instances().contents().front().templ == prior);
    CHECK(window.document()->write().value() == parentBytes);
}

TEST_CASE("instance visibility header control cycles and persists in viewsettings") {
    ui_test::app();
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const auto leaf = dir.filePath(QStringLiteral("leaf.vmf"));
    const auto map = dir.filePath(QStringLiteral("parent.vmf"));
    REQUIRE(writeFile(leaf, relay("leaf").toUtf8()));
    REQUIRE(writeFile(map, parentMap("leaf.vmf").toUtf8()));
    ui::MainWindow window;
    REQUIRE(window.openFile(map));
    auto* toggle = window.findChild<QToolButton*>(QStringLiteral("instanceVisibilityToggle"));
    REQUIRE(toggle);
    CHECK(window.editorSession().instanceVisibility() == document::InstanceVisibility::Normal);
    toggle->click();
    CHECK(window.editorSession().instanceVisibility() == document::InstanceVisibility::Hidden);
    auto* action = window.findChild<QAction*>(QStringLiteral("view.instances"));
    REQUIRE(action);
    action->trigger();
    CHECK(window.editorSession().instanceVisibility() == document::InstanceVisibility::Tinted);
    toggle->click();
    CHECK(window.editorSession().instanceVisibility() == document::InstanceVisibility::Normal);
    toggle->click();
    CHECK(window.editorSession().instanceVisibility() == document::InstanceVisibility::Hidden);
    CHECK(window.saveFile(map));
    ui::MainWindow reopened;
    REQUIRE(reopened.openFile(map));
    CHECK(reopened.editorSession().instanceVisibility() == document::InstanceVisibility::Hidden);
}
