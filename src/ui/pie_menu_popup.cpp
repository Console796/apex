#include "ui/pie_menu_popup.hpp"

#include "editor/pie_menus.hpp"
#include "ui/key_chord.hpp"
#include "ui/theme.hpp"
#include <QApplication>
#include <QHideEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QSettings>
#include <algorithm>
#include <array>
#include <cmath>

namespace apex::ui {
editor::PieSettings loadPieSettings() {
    editor::PieSettings result;
    QSettings settings;
    const auto number = [&](const char* name, double fallback, double minimum, double maximum) {
        bool ok = false;
        const double value = settings.value(QStringLiteral("input/pie/") + name, fallback).toDouble(&ok);
        return ok && std::isfinite(value) ? std::clamp(value, minimum, maximum) : fallback;
    };
    result.radius = number("radius", 100, 20, 500);
    result.deadZone = number("deadZone", 12, 0, 500);
    result.tapTimeout = std::chrono::milliseconds(static_cast<int>(number("tapTimeout", 200, 0, 5000)));
    result.animation = std::chrono::milliseconds(static_cast<int>(number("animation", 60, 0, 5000)));
    result.confirmOnStopDistance = number("confirmOnStopDistance", 0, 0, 2000);
    return result;
}
void savePieSettings(const editor::PieSettings& value) {
    QSettings settings;
    settings.setValue("input/pie/radius", value.radius);
    settings.setValue("input/pie/deadZone", value.deadZone);
    settings.setValue("input/pie/tapTimeout", static_cast<int>(value.tapTimeout.count()));
    settings.setValue("input/pie/animation", static_cast<int>(value.animation.count()));
    settings.setValue("input/pie/confirmOnStopDistance", value.confirmOnStopDistance);
}

PieMenuPopup::PieMenuPopup(editor::EditorSession& session, QWidget* parent)
    : QWidget(parent, Qt::Popup | Qt::FramelessWindowHint), session_(session) {
    setObjectName(QStringLiteral("pieMenuPopup"));
    setAttribute(Qt::WA_TranslucentBackground);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    qApp->installEventFilter(this);
    timer_.setInterval(8);
    connect(&timer_, &QTimer::timeout, this, [this] { session_.tickPie(); sync(); });
}
QString PieMenuPopup::label(std::size_t index) const {
    const auto& item = editor::toolPie().items[index];
    const auto slot = *item.slot;
    constexpr std::array digits{8, 9, 6, 3, 2, 1, 4, 7};
    const auto shortcut = shortcutText(session_.preset(), item.action);
    return QStringLiteral("%1%2\n%3  [%4]")
        .arg(session_.activeTool() == item.action ? QStringLiteral("✓ ") : QString(),
             QString::fromStdString(item.label),
             shortcut)
        .arg(digits[static_cast<std::size_t>(slot)]);
}
void PieMenuPopup::open(QWidget* pane) {
    if (!session_.pie()) return;
    auto* top = pane->window();
    setParent(top, Qt::Popup | Qt::FramelessWindowHint);
    setGeometry(QRect(top->mapToGlobal(QPoint()), top->size()));
    const auto start = session_.pie()->centre();
    origin_ = pane->mapTo(top, QPoint());
    const QPointF requested = origin_ + QPointF(start.x, start.y);
    labelSize_ = QSizeF(0, fontMetrics().height() * 2 + 16);
    for (std::size_t i = 0; i < editor::toolPie().items.size(); ++i)
        for (const auto& line : label(i).split('\n'))
            labelSize_.setWidth(std::max(labelSize_.width(), double(fontMetrics().horizontalAdvance(line) + 24)));
    const double radius = std::max(session_.pieSettings().radius, session_.pieSettings().deadZone);
    const auto clamp = [](double value, double extent, double size) {
        return size < extent * 2 ? size / 2 : std::clamp(value, extent, size - extent);
    };
    centre_ = {clamp(requested.x(), radius + labelSize_.width() + 4, width()),
               clamp(requested.y(), radius + labelSize_.height() + 4, height())};
    displacement_ = centre_ - requested;
    cursor_ = requested;
    show();
    if (!session_.pie()) { sync(); return; }
    activateWindow();
    setFocus();
    grabKeyboard();
    timer_.start();
}
QRectF PieMenuPopup::drawingBounds() const {
    const double radius = std::max(session_.pieSettings().radius, session_.pieSettings().deadZone);
    return QRectF(centre_.x() - radius - labelSize_.width(),
                  centre_.y() - radius - labelSize_.height(),
                  2 * (radius + labelSize_.width()), 2 * (radius + labelSize_.height()));
}
void PieMenuPopup::sync() {
    if (session_.pie()) { update(); return; }
    timer_.stop();
    closing_ = true;
    releaseKeyboard();
    hide();
    closing_ = false;
}
bool PieMenuPopup::event(QEvent* e) {
    if (e->type() == QEvent::ShortcutOverride) { e->accept(); return true; }
    return QWidget::event(e);
}
bool PieMenuPopup::eventFilter(QObject* watched, QEvent* e) {
    if (isVisible() && ((e->type() == QEvent::WindowDeactivate && (watched == this || watched == parentWidget())) ||
        (e->type() == QEvent::ApplicationStateChange && QGuiApplication::applicationState() != Qt::ApplicationActive))) {
        session_.pieFocusLost();
        sync();
    }
    return false;
}
void PieMenuPopup::hideEvent(QHideEvent*) {
    if (!closing_) { session_.pieFocusLost(); timer_.stop(); }
}
void PieMenuPopup::send(editor::InputEvent event) {
    if (!session_.pieSource()) return;
    event.source = *session_.pieSource();
    const auto position = cursor_ - origin_ - displacement_;
    event.cursor = {position.x(), position.y()};
    session_.input(event, {});
    sync();
}
void PieMenuPopup::mouse(QMouseEvent* e) {
    cursor_ = e->position();
    editor::InputEvent event;
    event.kind = e->type() == QEvent::MouseButtonPress ? editor::InputKind::Press :
                 e->type() == QEvent::MouseButtonRelease ? editor::InputKind::Release : editor::InputKind::Move;
    event.button = e->button() == Qt::LeftButton ? editor::PointerButton::Left :
                   e->button() == Qt::RightButton ? editor::PointerButton::Right :
                   e->button() == Qt::MiddleButton ? editor::PointerButton::Middle : editor::PointerButton::None;
    event.left = e->buttons().testFlag(Qt::LeftButton);
    event.right = e->buttons().testFlag(Qt::RightButton);
    event.middle = e->buttons().testFlag(Qt::MiddleButton);
    event.anyButton = e->buttons() != Qt::NoButton;
    event.ctrl = e->modifiers().testFlag(Qt::ControlModifier);
    event.shift = e->modifiers().testFlag(Qt::ShiftModifier);
    event.alt = e->modifiers().testFlag(Qt::AltModifier);
    send(event);
    e->accept();
}
void PieMenuPopup::key(QKeyEvent* e) {
    editor::InputEvent event;
    event.kind = e->type() == QEvent::KeyPress ? editor::InputKind::KeyDown : editor::InputKind::KeyUp;
    event.chord = keyChord(*e);
    event.autoRepeat = e->isAutoRepeat();
    event.ctrl = e->modifiers().testFlag(Qt::ControlModifier);
    event.shift = e->modifiers().testFlag(Qt::ShiftModifier);
    event.alt = e->modifiers().testFlag(Qt::AltModifier);
    send(event);
    e->accept();
}
void PieMenuPopup::mouseMoveEvent(QMouseEvent* e) { mouse(e); }
void PieMenuPopup::mousePressEvent(QMouseEvent* e) { mouse(e); }
void PieMenuPopup::mouseReleaseEvent(QMouseEvent* e) { mouse(e); }
void PieMenuPopup::keyPressEvent(QKeyEvent* e) { key(e); }
void PieMenuPopup::keyReleaseEvent(QKeyEvent* e) { key(e); }
void PieMenuPopup::paintEvent(QPaintEvent*) {
    if (!session_.pie()) return;
    const auto& tokens = currentTokens();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    auto wash = tokens.window;
    wash.setAlpha(40);
    p.fillRect(rect(), wash);
    const double progress = session_.pie()->openProgress(session_.pieTime());
    p.setOpacity(progress);
    p.translate(centre_);
    p.scale(0.8 + 0.2 * progress, 0.8 + 0.2 * progress);
    const auto& settings = session_.pieSettings();
    p.setPen(QPen(tokens.border, 2));
    auto fill = tokens.panel; fill.setAlpha(180);
    p.setBrush(fill);
    p.drawEllipse(QPointF(), settings.radius, settings.radius);
    p.setPen(QPen(tokens.textDim, 1));
    p.setBrush(tokens.base);
    p.drawEllipse(QPointF(), settings.deadZone, settings.deadZone);
    p.setBrush(tokens.text);
    p.drawEllipse(QPointF(), 2, 2);
    const auto highlighted = session_.pie()->highlighted();
    for (std::size_t i = 0; i < editor::toolPie().items.size(); ++i) {
        const auto& item = editor::toolPie().items[i];
        const auto direction = editor::slotDirection(*item.slot);
        const QPointF position(direction.x * settings.radius, direction.y * settings.radius);
        const QRectF box(position.x() - (direction.x < 0 ? labelSize_.width() : direction.x == 0 ? labelSize_.width() / 2 : 0),
                         position.y() - (direction.y < 0 ? labelSize_.height() : direction.y == 0 ? labelSize_.height() / 2 : 0),
                         labelSize_.width(), labelSize_.height());
        const bool enabled = session_.isToolEnabled(item.action);
        p.setOpacity(progress * (enabled ? 1 : 0.4));
        p.setPen(QPen(highlighted == i ? tokens.accent : tokens.border, highlighted == i ? 2 : 1));
        p.setBrush(highlighted == i ? tokens.selection : tokens.raised);
        p.drawRoundedRect(box, tokens.radius, tokens.radius);
        p.setPen(tokens.text);
        p.drawText(box, Qt::AlignCenter, label(i));
    }
}
} // namespace apex::ui
