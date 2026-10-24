#include "test_app.hpp"
#include "../editor/fixtures.hpp"
#include "ui/main_window.hpp"
#include "ui/settings_dialog.hpp"

#include <QAction>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QInputDialog>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QTimer>

using namespace editor_test;
TEST_CASE("topology UI actions preferences and merge confirmation") {
    apex::ui_test::app();
    QSettings().remove("editing/confirmNonConvexMerge");
    apex::ui::MainWindow window;
    auto& session = window.editorSession();
    REQUIRE(session.confirmNonConvexMerge);
    for (const auto* name : {"edit.carve", "edit.hollow", "edit.merge"})
        REQUIRE(window.findChild<QAction*>(QString::fromLatin1(name)));
    Document doc{parsed()};
    const auto a = addBox(doc, {0, 0, 0}, {128, 64, 64});
    const auto b = addBox(doc, {0, 64, 0}, {64, 128, 64});
    session.attach(doc);
    session.selection().setMode(SelectionMode::Solids);
    session.selection().add(id(doc, a));
    session.selection().add(id(doc, b));
    const auto before = *doc.write();
    QTimer::singleShot(0, [&] {
        auto* box = window.findChild<QMessageBox*>("confirmNonConvexMerge");
        REQUIRE(box);
        CHECK(box->text().contains("units³"));
        CHECK_FALSE(session.preview().ghosts().empty());
        box->button(QMessageBox::Cancel)->click();
    });
    REQUIRE(session.perform("edit.merge"));
    CHECK(*doc.write() == before);
    QTimer::singleShot(0, [&] {
        auto* box = window.findChild<QMessageBox*>("confirmNonConvexMerge");
        REQUIRE(box);
        box->checkBox()->setChecked(true);
        for (auto* button : box->buttons()) if (box->buttonRole(button) == QMessageBox::AcceptRole) button->click();
    });
    REQUIRE(session.perform("edit.merge"));
    CHECK_FALSE(session.confirmNonConvexMerge);
    CHECK_FALSE(QSettings().value("editing/confirmNonConvexMerge", true).toBool());
    CHECK(session.preview().empty());
    {
        apex::ui::SettingsDialog prefs(window);
        auto* confirm = prefs.findChild<QCheckBox*>("settingsConfirmNonConvexMerge");
        REQUIRE(confirm);
        CHECK_FALSE(confirm->isChecked());
        confirm->setChecked(true);
        prefs.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Apply)->click();
        CHECK(session.confirmNonConvexMerge);
        CHECK(QSettings().value("editing/confirmNonConvexMerge").toBool());
    }
    QTimer::singleShot(0, [&] {
        auto* prompt = window.findChild<QInputDialog*>();
        REQUIRE(prompt);
        prompt->setDoubleValue(-16);
        prompt->accept();
    });
    REQUIRE(session.perform("edit.hollow"));
    CHECK(doc.undoLabels().back() == "Hollow solids");
    session.detach();
    QSettings().remove("editing/confirmNonConvexMerge");
}
