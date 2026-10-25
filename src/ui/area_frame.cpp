#include "ui/area_frame.hpp"

#include <QActionGroup>
#include <QCoreApplication>
#include <QHBoxLayout>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QSettings>
#include <QToolButton>
#include <QVBoxLayout>

#include "ui/history_panel.hpp"
#include "ui/inspector_panel.hpp"
#include "ui/log_panel.hpp"
#include "ui/materials_panel.hpp"
#include "ui/game_content.hpp"
#include "ui/face_properties_panel.hpp"
#include "ui/outliner_panel.hpp"
#include "ui/theme.hpp"
#include "ui/visgroup_panel.hpp"

namespace apex::ui {

using viewport::AreaRole;

QString roleTitle(AreaRole role) {
    const auto tr = [](const char* text) { return QCoreApplication::translate("apex::ui::areas", text); };
    switch (role) {
    case AreaRole::Viewport: return tr("Viewport");
    case AreaRole::Outliner: return tr("Outliner");
    case AreaRole::Inspector: return tr("Inspector");
    case AreaRole::Visgroups: return tr("Visgroups");
    case AreaRole::History: return tr("Undo History");
    case AreaRole::Log: return tr("Log");
    case AreaRole::Materials: return tr("Materials");
    case AreaRole::FaceProperties: return tr("Face Properties");
    }
    return {};
}

namespace {
template <typename F> void withPanel(QWidget* panel, F&& f) {
    if (auto* p = qobject_cast<OutlinerPanel*>(panel)) f(*p);
    else if (auto* p = qobject_cast<InspectorPanel*>(panel)) f(*p);
    else if (auto* p = qobject_cast<VisgroupPanel*>(panel)) f(*p);
    else if (auto* p = qobject_cast<HistoryPanel*>(panel)) f(*p);
    else if (auto* p = qobject_cast<MaterialsPanel*>(panel)) f(*p);
    else if (auto* p = qobject_cast<FacePropertiesPanel*>(panel)) f(*p);
}
}  // namespace

void setPanelSession(QWidget* panel, editor::EditorSession* session) {
    withPanel(panel, [session](auto& p) { p.setEditorSession(session); });
}

void setPanelDocument(QWidget* panel, document::Document* document) {
    withPanel(panel, [document](auto& p) { p.setDocument(document); });
}

void setPanelGameContent(QWidget* panel, GameContent* content) {
    if (auto* p = qobject_cast<MaterialsPanel*>(panel)) p->setGameContent(content);
}

void bindPanel(QWidget* panel, document::Document* document, editor::EditorSession* session) {
    // Session first, as the main window binds them.
    setPanelSession(panel, session);
    if (document) setPanelDocument(panel, document);
}

QWidget* createPanel(AreaRole role, document::Document* document, editor::EditorSession* session, GameContent* content) {
    QWidget* panel = nullptr;
    switch (role) {
    case AreaRole::Viewport: return nullptr;
    case AreaRole::Outliner: panel = new OutlinerPanel; break;
    case AreaRole::Inspector: panel = new InspectorPanel; break;
    case AreaRole::Visgroups: panel = new VisgroupPanel; break;
    case AreaRole::History: panel = new HistoryPanel; break;
    case AreaRole::Log: panel = new LogPanel; break;
    case AreaRole::Materials: panel = new MaterialsPanel; break;
    case AreaRole::FaceProperties: panel = new FacePropertiesPanel; break;
    }
    panel->setProperty("areaRole", QString::fromLatin1(viewport::nameOf(role)));
    bindPanel(panel, document, session);
    setPanelGameContent(panel, content);
    return panel;
}

QToolButton* makeRoleButton(AreaRole current, std::function<void(AreaRole)> choose, QWidget* parent) {
    auto* button = new QToolButton(parent);
    button->setObjectName(QStringLiteral("AreaRoleButton"));
    button->setText(roleTitle(current) + QStringLiteral(" ▾"));
    button->setToolTip(QCoreApplication::translate("apex::ui::areas", "Area type: what this area shows"));
    button->setPopupMode(QToolButton::InstantPopup);
    button->setAutoRaise(true);
    auto* menu = new QMenu(button);
    menu->setObjectName(QStringLiteral("AreaRoleMenu"));
    auto* group = new QActionGroup(menu);
    for (const auto role : viewport::kAreaRoles) {
        auto* action = menu->addAction(roleTitle(role));
        action->setCheckable(true);
        action->setChecked(role == current);
        action->setData(QString::fromLatin1(viewport::nameOf(role)));
        group->addAction(action);
        QObject::connect(action, &QAction::triggered, button, [choose, role, current] {
            if (role != current) choose(role);
        });
    }
    button->setMenu(menu);
    return button;
}

QToolButton* makeCollapseButton(std::function<void()> collapse, QWidget* parent) {
    auto* button = new QToolButton(parent);
    button->setObjectName(QStringLiteral("AreaCollapseButton"));
    button->setText(QStringLiteral("–"));
    button->setToolTip(QCoreApplication::translate("apex::ui::areas", "Collapse this area to a strip"));
    button->setAutoRaise(true);
    QObject::connect(button, &QToolButton::clicked, button, [collapse] { collapse(); });
    return button;
}

void addAreaActions(QMenu& menu, const AreaMenuActions& a) {
    const auto tr = [](const char* text) { return QCoreApplication::translate("apex::ui::areas", text); };
    menu.addAction(tr("Split horizontally (side by side)"), [split = a.split] { split(viewport::Orientation::Horizontal); });
    menu.addAction(tr("Split vertically (stacked)"), [split = a.split] { split(viewport::Orientation::Vertical); });
    menu.addSeparator();
    menu.addAction(a.maximized ? tr("Restore") : tr("Maximize"), a.maximize);
    if (a.popOut) menu.addAction(tr("Open in new window"), a.popOut);
    auto* close = menu.addAction(tr("Close area"), a.close);
    close->setEnabled(a.canClose);
}

PanelFrame::PanelFrame(viewport::NodeId pane, AreaRole role, QWidget* panel, bool maximized, bool canClose,
                       QWidget* parent)
    : QFrame(parent), pane_(pane), panel_(panel) {
    setObjectName(QStringLiteral("AreaFrame"));
    setProperty("areaRole", QString::fromLatin1(viewport::nameOf(role)));
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(1, 1, 1, 1);
    column->setSpacing(0);

    auto* header = new QWidget;
    header->setObjectName(QStringLiteral("AreaHeader"));
    auto* row = new QHBoxLayout(header);
    row->setContentsMargins(2, 2, 2, 2);
    row->addWidget(makeRoleButton(role, [this](AreaRole r) { emit roleRequested(pane_, r); }, header));
    row->addStretch(1);
    row->addWidget(makeCollapseButton([this] { emit collapseRequested(pane_); }, header));
    auto* menuButton = new QToolButton(header);
    menuButton->setObjectName(QStringLiteral("AreaMenuButton"));
    menuButton->setText(QString(QChar(0x2630)));
    menuButton->setToolTip(tr("Area options"));
    menuButton->setPopupMode(QToolButton::InstantPopup);
    menuButton->setAutoRaise(true);
    auto* menu = new QMenu(menuButton);
    menu->setObjectName(QStringLiteral("AreaMenu"));
    addAreaActions(*menu, {[this](viewport::Orientation o) { emit splitRequested(pane_, o); },
                           [this] { emit maximizeToggled(pane_); },
                           [this, role] { emit popOutRequested(role); },
                           [this] { emit closeRequested(pane_); }, maximized, canClose});
    menuButton->setMenu(menu);
    row->addWidget(menuButton);
    column->addWidget(header);
    column->addWidget(panel_, 1);
    panel_->show();
}

CollapsedStrip::CollapsedStrip(viewport::NodeId pane, AreaRole role, bool vertical, QWidget* parent)
    : QWidget(parent), pane_(pane), text_(roleTitle(role)), vertical_(vertical) {
    setObjectName(QStringLiteral("AreaStrip"));
    setProperty("areaRole", QString::fromLatin1(viewport::nameOf(role)));
    setToolTip(tr("%1 (collapsed): click to expand").arg(text_));
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_Hover);
    if (vertical_) setFixedWidth(kThickness);
    else setFixedHeight(kThickness);
}

void CollapsedStrip::paintEvent(QPaintEvent*) {
    const Tokens& t = currentTokens();
    QPainter p(this);
    p.fillRect(rect(), underMouse() ? t.raised : t.panel);
    p.setPen(t.border);
    p.drawRect(rect().adjusted(0, 0, -1, -1));
    p.setPen(t.textDim);
    const QString label = QStringLiteral("▸ ") + text_;
    if (vertical_) {
        p.translate(width() / 2.0, 8);
        p.rotate(90);
        p.drawText(QRectF(0, -kThickness / 2.0, height() - 16, kThickness), Qt::AlignLeft | Qt::AlignVCenter, label);
    } else {
        p.drawText(rect().adjusted(8, 0, -8, 0), Qt::AlignLeft | Qt::AlignVCenter, label);
    }
}

bool CollapsedStrip::event(QEvent* event) {
    if (event->type() == QEvent::HoverEnter || event->type() == QEvent::HoverLeave) update();
    return QWidget::event(event);
}

void CollapsedStrip::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) emit expandRequested(pane_);
    QWidget::mouseReleaseEvent(event);
}

FloatingPanel::FloatingPanel(AreaRole role, QWidget* panel, QWidget* parent)
    : QWidget(parent, Qt::Tool), role_(role), panel_(panel) {
    setObjectName(QStringLiteral("FloatingPanel_") + QString::fromLatin1(viewport::nameOf(role)));
    setWindowTitle(roleTitle(role));
    setAttribute(Qt::WA_DeleteOnClose, false);
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(4, 4, 4, 4);
    column->addWidget(panel_);
    resize(360, 480);
    if (const auto geometry = QSettings().value(QStringLiteral("ui/floating/") + QString::fromLatin1(viewport::nameOf(role)));
        geometry.isValid())
        restoreGeometry(geometry.toByteArray());
}

void FloatingPanel::hideEvent(QHideEvent* event) {
    QSettings().setValue(QStringLiteral("ui/floating/") + QString::fromLatin1(viewport::nameOf(role_)), saveGeometry());
    QWidget::hideEvent(event);
}

}  // namespace apex::ui
