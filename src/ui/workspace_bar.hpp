#pragma once

#include <QWidget>

#include "viewport/workspace.hpp"

class QTabBar;
class QToolButton;

namespace apex::ui {

class ViewportArea;

// Workspace tabs above the area tree (0.2.12, Blender's workspace tabs). The bar owns the
// WorkspaceSet; the area shows the active workspace's layout, which is written back before a
// switch and on save. Tabs can be reordered by dragging; their context menu renames, duplicates,
// resets and deletes; "+" adds one from a template.
class WorkspaceBar final : public QWidget {
    Q_OBJECT
public:
    explicit WorkspaceBar(ViewportArea& area, QWidget* parent = nullptr);

    // QSettings "ui/workspaces"; a missing or unreadable value gives the default set.
    void load();
    void save();

    const viewport::WorkspaceSet& workspaces();  // Current area layout written back first.
    std::size_t active() const { return set_.active; }
    void switchTo(std::size_t index);
    void step(int delta);  // Next/previous, wrapping.
    void addFromTemplate(std::string_view name);
    void duplicate(std::size_t index);
    void rename(std::size_t index, const QString& name);
    void remove(std::size_t index);
    void resetActive();  // The active workspace back to its template.

signals:
    void switched();

private:
    void capture();
    void syncTabs();
    void showTabMenu(const QPoint& pos);
    void promptRename(std::size_t index);

    ViewportArea& area_;
    viewport::WorkspaceSet set_;
    QTabBar* tabs_ = nullptr;
    QToolButton* add_ = nullptr;
    bool syncing_ = false;
};

}  // namespace apex::ui
