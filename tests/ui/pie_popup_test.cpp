#include "test_app.hpp"
#include <doctest/doctest.h>
#include "ui/main_window.hpp"
#include "ui/pie_menu_popup.hpp"
#include "ui/settings_dialog.hpp"
#include "ui/transform_input.hpp"
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QMessageBox>
#include <QTimer>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStatusBar>

using namespace apex;
using namespace std::chrono_literals;
namespace {
class PiePane : public QWidget {
public:
    ui::TransformInput input;
    bool navigating = false;
    explicit PiePane(ui::MainWindow& window) : QWidget(&window) {
        resize(800, 600);
        setFocusPolicy(Qt::StrongFocus);
        input.setWidget(this);
        input.setSession(&window.editorSession());
        show();
        setFocus();
    }
    void keyPressEvent(QKeyEvent* e) override { input.key(*e, {}, size(), !navigating); }
    void keyReleaseEvent(QKeyEvent* e) override { input.key(*e, {}, size(), !navigating); }
    void mouseMoveEvent(QMouseEvent* e) override { input.mouse(*e, {}, size()); }
};
void key(QWidget& target, int value, bool down = true) {
    QKeyEvent e(down ? QEvent::KeyPress : QEvent::KeyRelease, value, Qt::NoModifier);
    QCoreApplication::sendEvent(&target, &e);
}
void mouse(QWidget& target, QPointF position, bool click = false) {
    QMouseEvent e(click ? QEvent::MouseButtonPress : QEvent::MouseMove, position,
                  target.mapToGlobal(position), click ? Qt::LeftButton : Qt::NoButton,
                  click ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&target, &e);
}
struct Fixture {
    ui::MainWindow window;
    PiePane pane{window};
    std::chrono::milliseconds now{0};
    Fixture() {
        window.resize(1200, 800);
        window.applyPreset(editor::hammerPreset());
        window.editorSession().setClock([this] { return now; });
        auto settings = editor::PieSettings{};
        window.editorSession().setPieSettings(settings);
        window.show();
        window.activateWindow();
        pane.show();
        pane.setFocus();
        QCoreApplication::processEvents();
    }
    ~Fixture() { window.editorSession().setClock([] { return 0ms; }); }
    ui::PieMenuPopup* open(QPointF position = {400, 300}) {
        mouse(pane, position);
        key(pane, Qt::Key_Q);
        return window.findChild<ui::PieMenuPopup*>();
    }
};
}
TEST_CASE("pie popup holds and releases over E to pick Block") {
    ui_test::app();
    Fixture f;
    f.window.newFile();
    auto* popup = f.open();
    REQUIRE(popup);
    REQUIRE(popup->isVisible());
    REQUIRE(f.window.editorSession().pie());
    mouse(*popup, popup->centre() + QPointF(100, 0));
    f.now = 250ms;
    key(*popup, Qt::Key_Q, false);
    CHECK(f.window.editorSession().activeTool() == "tool.block");
    CHECK_FALSE(popup->isVisible());
    CHECK_FALSE(f.window.editorSession().pie());
}
TEST_CASE("pie popup tap pane release before event processing and click or dead zone") {
    ui_test::app();
    Fixture f;
    f.window.newFile();
    class BeforeShow : public QObject {
    public:
        PiePane& pane;
        std::chrono::milliseconds& now;
        bool released = false;
        BeforeShow(PiePane& p, std::chrono::milliseconds& n) : pane(p), now(n) {}
        bool eventFilter(QObject* object, QEvent* event) override {
            if (!released && event->type() == QEvent::Show && qobject_cast<ui::PieMenuPopup*>(object)) {
                released = true;
                now = 30ms;
                key(pane, Qt::Key_Q, false);
            }
            return false;
        }
    } beforeShow(f.pane, f.now);
    ui_test::app().installEventFilter(&beforeShow);
    auto* popup = f.open();
    ui_test::app().removeEventFilter(&beforeShow);
    REQUIRE(popup);
    CHECK(beforeShow.released);
    REQUIRE(f.window.editorSession().pie());
    CHECK(f.window.editorSession().pie()->clickStyle());
    mouse(*popup, popup->centre() + QPointF(100, 0), true);
    CHECK(f.window.editorSession().activeTool() == "tool.block");
    REQUIRE_FALSE(f.window.editorSession().pie());
    popup = f.open();
    f.now += 30ms;
    key(f.pane, Qt::Key_Q, false);
    mouse(*popup, popup->centre(), true);
    CHECK_FALSE(f.window.editorSession().pie());
    CHECK_FALSE(popup->isVisible());
}
TEST_CASE("pie popup click closes while Q held and later pane release is consumed") {
    ui_test::app();
    Fixture f;
    f.window.newFile();
    auto* popup = f.open();
    REQUIRE(popup);
    mouse(*popup, popup->centre() + QPointF(100, 0), true);
    REQUIRE_FALSE(f.window.editorSession().pie());
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_Q, Qt::NoModifier);
    CHECK(f.pane.input.key(release, {}, f.pane.size(), true));
    CHECK_FALSE(f.pane.input.key(release, {}, f.pane.size(), true));
}
TEST_CASE("pie popup cancels on popup or top-level deactivation and Qt hide") {
    ui_test::app();
    for (int how : {0, 1, 2}) {
        Fixture f;
        auto* popup = f.open();
        REQUIRE(popup);
        if (how == 2) popup->hide();
        else {
            QEvent event(QEvent::WindowDeactivate);
            QCoreApplication::sendEvent(how == 0 ? static_cast<QWidget*>(popup) : &f.window, &event);
        }
        CHECK_FALSE(f.window.editorSession().pie());
        CHECK_FALSE(popup->isVisible());
    }
}
TEST_CASE("pie popup without document only permits Select and clamps corner direction") {
    ui_test::app();
    Fixture f;
    auto* popup = f.open({1, 1});
    REQUIRE(popup);
    CHECK(QRectF(popup->rect()).contains(popup->drawingBounds()));
    key(f.pane, Qt::Key_Q, false);
    mouse(*popup, popup->centre() + QPointF(100, 0));
    CHECK_FALSE(f.window.editorSession().pie()->highlighted());
    mouse(*popup, popup->centre() + QPointF(100, 0), true);
    REQUIRE(f.window.editorSession().pie());
    mouse(*popup, popup->centre() - QPointF(100, 0), true);
    CHECK_FALSE(f.window.editorSession().pie());
    CHECK(f.window.editorSession().activeTool() == "tool.select");
    f.window.newFile();
    popup = f.open({1, 1});
    mouse(*popup, popup->centre() + QPointF(100, 0));
    REQUIRE(f.window.editorSession().pie()->highlighted());
    CHECK(*f.window.editorSession().pie()->highlighted() == 4);
    f.now += 250ms;
    key(*popup, Qt::Key_Q, false);
    CHECK(f.window.editorSession().activeTool() == "tool.block");
}
TEST_CASE("pie refusal while navigating reaches status bar") {
    ui_test::app();
    Fixture f;
    f.pane.navigating = true;
    key(f.pane, Qt::Key_Q);
    CHECK_FALSE(f.window.editorSession().pie());
    CHECK(f.window.statusBar()->currentMessage() == QString::fromStdString(f.window.editorSession().pieRefusal()));
    CHECK_FALSE(f.window.statusBar()->currentMessage().isEmpty());
}
TEST_CASE("pie Input preferences apply persist reload and cancel") {
    ui_test::app();
    ui::MainWindow window;
    const auto original = window.editorSession().pieSettings();
    {
        ui::SettingsDialog dialog(window);
        dialog.findChild<QDoubleSpinBox*>("settingsPieRadius")->setValue(135.5);
        dialog.findChild<QDoubleSpinBox*>("settingsPieDeadZone")->setValue(16);
        dialog.findChild<QDoubleSpinBox*>("settingsPieConfirmOnStopDistance")->setValue(180);
        dialog.findChild<QSpinBox*>("settingsPieTapTimeout")->setValue(280);
        dialog.findChild<QSpinBox*>("settingsPieAnimation")->setValue(0);
        dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Apply)->click();
        CHECK(window.editorSession().pieSettings().radius == 135.5);
        CHECK(window.editorSession().pieSettings().deadZone == 16);
        CHECK(window.editorSession().pieSettings().tapTimeout == 280ms);
        CHECK(window.editorSession().pieSettings().animation == 0ms);
        CHECK(window.editorSession().pieSettings().confirmOnStopDistance == 180);
        CHECK(QSettings().value("input/pie/radius").toDouble() == 135.5);
    }
    {
        ui::MainWindow reloaded;
        CHECK(reloaded.editorSession().pieSettings().radius == 135.5);
        ui::SettingsDialog dialog(reloaded);
        CHECK(dialog.findChild<QSpinBox*>("settingsPieAnimation")->value() == 0);
        dialog.findChild<QDoubleSpinBox*>("settingsPieRadius")->setValue(200);
        QTimer::singleShot(0, [] {
            auto* question = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            REQUIRE(question);
            question->done(QMessageBox::Yes);
        });
        dialog.reject();
        CHECK(reloaded.editorSession().pieSettings().radius == 135.5);
    }
    ui::savePieSettings(original);
}
