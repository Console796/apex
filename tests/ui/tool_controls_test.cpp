#include "test_app.hpp"
#include <doctest/doctest.h>
#include "ui/main_window.hpp"
#include "ui/transform_input.hpp"
#include "ui/outliner_panel.hpp"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QSettings>
#include <QToolBar>
#include <QToolButton>
#include <QWidgetAction>

using namespace apex;

TEST_CASE("Tools drop-down shares actions and follows active tool and options") {
    ui_test::app();
    ui::MainWindow window;
    auto* tools = window.findChild<QToolBar*>("toolbarTools");
    auto* transform = window.findChild<QToolBar*>("toolbarTransform");
    auto* button = window.findChild<QToolButton*>("toolDropDown");
    auto* view = window.findChild<QMenu*>("menuView");
    REQUIRE(tools); REQUIRE(transform); REQUIRE(button); REQUIRE(view);
    CHECK(button->popupMode() == QToolButton::InstantPopup);
    REQUIRE(button->menu()->actions().size() == 8);
    for (auto* action : button->menu()->actions()) {
        CHECK(view->actions().contains(action));
        CHECK_FALSE(transform->actions().contains(action));
        CHECK(action->isCheckable());
        CHECK(action->isEnabled() == (action->objectName() == "tool.select"));
    }
    auto* picker = window.findChild<QComboBox*>("entityClassPicker");
    auto* align = window.findChild<QCheckBox*>("entityAlignToSurface");
    REQUIRE(picker); REQUIRE(align);
    window.newFile();
    for (auto* action : button->menu()->actions()) {
        REQUIRE(action->isEnabled());
        action->trigger();
        CHECK(window.editorSession().activeTool() == action->objectName().toStdString());
        CHECK(action->isChecked());
        CHECK(button->text() == action->text());
        int checked = 0;
        for (auto* other : button->menu()->actions()) checked += other->isChecked();
        CHECK(checked == 1);
        const bool entity = action->objectName() == "tool.entity";
        // An unexposed offscreen window never runs the toolbar's layout pass, which shows the widgets;
        // check the options' actions instead.
        int entityOptions = 0;
        for (auto* option : tools->actions()) {
            auto* widgetAction = qobject_cast<QWidgetAction*>(option);
            if (!widgetAction || widgetAction->defaultWidget() == button) continue;
            if (widgetAction->defaultWidget() == picker || widgetAction->defaultWidget() == align) {
                ++entityOptions;
                CHECK(widgetAction->isVisible() == entity);
            }
            else CHECK(widgetAction->isVisible() == (action->objectName() == "tool.vertex"));
        }
        CHECK(entityOptions == 2);
    }
    REQUIRE(window.editorSession().perform("tool.entity"));
    CHECK(button->text() == window.findChild<QAction*>("tool.entity")->text());
}

TEST_CASE("selection icons track preset shortcuts and theme text colour") {
    ui_test::app();
    ui::MainWindow window;
    auto* bar = window.findChild<QToolBar*>("toolbarSelection");
    REQUIRE(bar);
    CHECK(bar->toolButtonStyle() == Qt::ToolButtonIconOnly);
    window.applyPreset(editor::hammerPreset());
    int modes = 0;
    for (auto* action : bar->actions()) {
        if (action->isSeparator()) continue;
        CHECK_FALSE(action->icon().isNull());
        const auto* entry = editor::actionRegistry().find(action->objectName().toStdString());
        REQUIRE(entry);
        CHECK(action->toolTip().startsWith(QString::fromStdString(entry->name)));
        CHECK(action->toolTip().contains(QString::fromStdString(entry->description)));
        if (action->objectName().startsWith("selection.mode.")) {
            ++modes;
            CHECK(action->toolTip().contains(QString("(%1)").arg(modes)));
        }
        for (const auto& shortcut : action->shortcuts())
            CHECK(action->toolTip().contains(shortcut.toString(QKeySequence::NativeText)));
    }
    CHECK(modes == 4);
    auto* groups = window.findChild<QAction*>("selection.mode.groups");
    auto custom = editor::hammerPreset();
    custom.bindings.at("selection.mode.groups").value = std::vector<editor::KeyChord>{{"9"}};
    window.applyPreset(custom);
    CHECK(groups->toolTip().contains("(9)"));
    CHECK_FALSE(groups->toolTip().contains("(1)"));
    window.applyPreset(editor::blenderPreset());
    CHECK(groups->toolTip().contains("(1)"));
    auto* ignore = window.findChild<QAction*>("selection.ignore_groups");
    CHECK_FALSE(ignore->toolTip().contains("Ctrl+W"));
    const auto original = ui::currentTokens();
    auto tokens = original;
    tokens.text = QColor("#e23179");
    window.previewTokens(tokens);
    QCoreApplication::processEvents();
    const auto pixels = groups->icon().pixmap(24, 24).toImage();
    bool painted = false;
    for (int y = 0; y < pixels.height(); ++y) for (int x = 0; x < pixels.width(); ++x) {
        const auto colour = pixels.pixelColor(x, y);
        if (colour.alpha() == 255) {
            painted = true;
            CHECK(colour == tokens.text);
        }
    }
    CHECK(painted);
    window.previewTokens(original);
}

TEST_CASE("selection digits route through a focused pane and Q and digits type in outliner") {
    auto& app = ui_test::app();
    ui::MainWindow window;
    window.newFile();
    window.show();
    // Offscreen has no Vulkan surface; use the pane's production input adapter.
    class Pane : public QWidget {
    public:
        ui::TransformInput input;
        explicit Pane(QWidget* parent) : QWidget(parent) { setFocusPolicy(Qt::StrongFocus); }
        void keyPressEvent(QKeyEvent* event) override {
            if (!input.key(*event, viewport::Camera{}, QSizeF(800, 600), true))
                QWidget::keyPressEvent(event);
        }
    } pane(&window);
    pane.input.setSession(&window.editorSession());
    for (const auto* preset : {&editor::hammerPreset(), &editor::blenderPreset()}) {
        window.applyPreset(*preset);
        pane.show();
        window.activateWindow();
        pane.setFocus();
        QCoreApplication::processEvents();
        for (int digit = 1; digit <= 4; ++digit) {
            QKeyEvent event(QEvent::KeyPress, Qt::Key_0 + digit, Qt::NoModifier, QString::number(digit));
            app.sendEvent(&pane, &event);
            const char* modes[] = {"groups", "objects", "solids", "faces"};
            const auto id = QString("selection.mode.%1").arg(modes[digit - 1]);
            CHECK(window.findChild<QAction*>(id)->isChecked());
        }
        pane.hide();
        auto* filter = window.outliner()->findChild<QLineEdit*>("outlinerFilter");
        REQUIRE(filter);
        window.activateWindow();
        filter->setFocus();
        filter->clear();
        QCoreApplication::processEvents();
        for (int digit = 1; digit <= 4; ++digit) {
            QKeyEvent event(QEvent::KeyPress, Qt::Key_0 + digit, Qt::NoModifier, QString::number(digit));
            app.sendEvent(filter, &event);
        }
        QKeyEvent q(QEvent::KeyPress, Qt::Key_Q, Qt::NoModifier, "q");
        app.sendEvent(filter, &q);
        CHECK(filter->text() == "1234q");
        CHECK(window.findChild<QAction*>("selection.mode.faces")->isChecked());
        CHECK_FALSE(window.editorSession().pie());
    }
}

TEST_CASE("pie input trace receives open pick cancel and refusal with session timing") {
    ui_test::app();
    ui::MainWindow window;
    window.newFile();
    auto& session = window.editorSession();
    std::chrono::milliseconds now{10};
    session.setClock([&] { return now; });
    struct Entry { std::string event, detail; std::chrono::milliseconds time; };
    std::vector<Entry> trace;
    session.setPieTrace([&](std::string_view event, std::string_view detail, std::chrono::milliseconds time) {
        trace.push_back({std::string(event), std::string(detail), time});
    });
    window.applyPreset(editor::hammerPreset());
    editor::InputEvent event;
    event.kind = editor::InputKind::KeyDown;
    event.source = 1;
    event.chord = editor::KeyChord{"Q"};
    REQUIRE(session.input(event, {}));
    REQUIRE(trace.size() == 1);
    CHECK(trace.back().event == "open");
    CHECK(trace.back().time == now);
    now = std::chrono::milliseconds{60};
    event.chord = editor::KeyChord{"6"};
    REQUIRE(session.input(event, {}));
    REQUIRE(trace.size() == 2);
    CHECK(trace.back().event == "pick");
    CHECK(trace.back().detail == "tool.block");
    CHECK(trace.back().time == now);
    event.chord = editor::KeyChord{"Q"};
    REQUIRE(session.input(event, {}));
    session.pieFocusLost();
    REQUIRE(trace.size() == 4);
    CHECK(trace.back().event == "cancel");
    event.navigating = true;
    REQUIRE(session.input(event, {}));
    REQUIRE(trace.size() == 5);
    CHECK(trace.back().event == "refusal");
    CHECK(trace.back().detail == session.pieRefusal());
    // The callback's captures must outlive the session.
    session.setPieTrace({});
}
