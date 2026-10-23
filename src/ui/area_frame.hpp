#pragma once

#include <QFrame>
#include <QString>
#include <functional>

#include "viewport/layout.hpp"

class QHBoxLayout;
class QMenu;
class QToolButton;

namespace apex::document {
class Document;
}
namespace apex::editor {
class EditorSession;
}

namespace apex::ui {

// Area roles in the UI (0.2.12): the display name, the panel widget a non-viewport role shows, and
// the header controls every area shares (role switcher, collapse, area menu).
QString roleTitle(viewport::AreaRole role);
// A new panel for a non-viewport role (null for Viewport), already bound to the document/session.
QWidget* createPanel(viewport::AreaRole role, document::Document* document, editor::EditorSession* session);
void bindPanel(QWidget* panel, document::Document* document, editor::EditorSession* session);
void setPanelSession(QWidget* panel, editor::EditorSession* session);
void setPanelDocument(QWidget* panel, document::Document* document);

// The role switcher at the start of every area header (Blender's editor-type menu).
QToolButton* makeRoleButton(viewport::AreaRole current, std::function<void(viewport::AreaRole)> choose,
                            QWidget* parent);
QToolButton* makeCollapseButton(std::function<void()> collapse, QWidget* parent);
// Split, maximize, open in a new window (panels only) and close: the tail of every area menu.
struct AreaMenuActions {
    std::function<void(viewport::Orientation)> split;
    std::function<void()> maximize, popOut, close;
    bool maximized = false;
    bool canClose = true;
};
void addAreaActions(QMenu& menu, const AreaMenuActions& actions);

// A panel area: header (role, title, collapse, menu) above the panel widget, which it does not own
// across rebuilds (the area takes it back before deleting the frame).
class PanelFrame final : public QFrame {
    Q_OBJECT
public:
    PanelFrame(viewport::NodeId pane, viewport::AreaRole role, QWidget* panel, bool maximized, bool canClose,
               QWidget* parent = nullptr);
    viewport::NodeId paneId() const { return pane_; }
    QWidget* panel() const { return panel_; }

signals:
    void roleRequested(viewport::NodeId pane, viewport::AreaRole role);
    void collapseRequested(viewport::NodeId pane);
    void splitRequested(viewport::NodeId pane, viewport::Orientation orientation);
    void closeRequested(viewport::NodeId pane);
    void maximizeToggled(viewport::NodeId pane);
    void popOutRequested(viewport::AreaRole role);

private:
    viewport::NodeId pane_;
    QWidget* panel_;
};

// A collapsed area: a thin strip naming its role; a click expands it. `vertical` strips stand in a
// side-by-side split and draw their label rotated.
class CollapsedStrip final : public QWidget {
    Q_OBJECT
public:
    CollapsedStrip(viewport::NodeId pane, viewport::AreaRole role, bool vertical, QWidget* parent = nullptr);
    viewport::NodeId paneId() const { return pane_; }
    bool vertical() const { return vertical_; }
    static constexpr int kThickness = 22;

signals:
    void expandRequested(viewport::NodeId pane);

protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    viewport::NodeId pane_;
    QString text_;
    bool vertical_;
};

// A panel in its own top-level tool window ("Open in new window", or the floating inspector).
class FloatingPanel final : public QWidget {
    Q_OBJECT
public:
    FloatingPanel(viewport::AreaRole role, QWidget* panel, QWidget* parent);
    viewport::AreaRole role() const { return role_; }
    QWidget* panel() const { return panel_; }

protected:
    void hideEvent(QHideEvent* event) override;

private:
    viewport::AreaRole role_;
    QWidget* panel_;
};

}  // namespace apex::ui
