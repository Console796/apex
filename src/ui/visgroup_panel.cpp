#include "ui/visgroup_panel.hpp"

#include <QAction>
#include <QDropEvent>
#include <QDrag>
#include <QMimeData>
#include <QMenu>
#include <QPixmap>
#include <QSignalBlocker>
#include <QTreeWidgetItemIterator>
#include <sstream>

namespace apex::ui {
namespace {
constexpr auto visgroupMime = "application/x-apex-visgroup";
constexpr int autoRole = Qt::UserRole + 1;
constexpr int nameRole = Qt::UserRole + 2;
QString text(std::string_view s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }
QString identity(const QTreeWidgetItem* item) {
    if (!item) return {};
    if (item->data(0, Qt::UserRole).isValid()) return QStringLiteral("user:%1").arg(item->data(0, Qt::UserRole).toInt());
    return item->data(0, autoRole).toString();
}
}

VisgroupPanel::VisgroupPanel(QWidget* parent) : QTreeWidget(parent) {
    setHeaderHidden(true);
    setUniformRowHeights(true);
    setSelectionMode(SingleSelection);
    setEditTriggers(EditKeyPressed);
    setDragDropMode(InternalMove);
    setDefaultDropAction(Qt::MoveAction);
    setContextMenuPolicy(Qt::CustomContextMenu);
    const auto action = [this](const char* id, const QString& label, auto callback) {
        auto* a = new QAction(label, this);
        a->setObjectName(QString::fromLatin1(id));
        addAction(a);
        connect(a, &QAction::triggered, this, callback);
    };
    action("visgroup.new", tr("New"), [this] { newVisgroup(); });
    action("visgroup.new_child", tr("New child"), [this] { newVisgroup(true); });
    action("visgroup.rename", tr("Rename"), [this] { if (currentVisgroup()) editItem(currentItem()); });
    action("visgroup.delete", tr("Delete"), [this] {
        if (attached()) if (auto id = currentVisgroup()) session_->deleteVisgroup(*id);
    });
    action("visgroup.add", tr("Add selection"), [this] {
        if (attached()) if (auto id = currentVisgroup()) session_->addSelectionToVisgroup(*id);
    });
    action("visgroup.remove", tr("Remove selection"), [this] {
        if (attached()) if (auto id = currentVisgroup()) session_->removeSelectionFromVisgroup(*id);
    });
    action("visgroup.select", tr("Select members"), [this] {
        if (!attached() || !currentItem()) return;
        if (auto id = currentVisgroup()) session_->selectVisgroupMembers(*id);
        else if (const auto autoId = currentItem()->data(0, autoRole).toString(); !autoId.isEmpty() && autoId != "Auto" && autoId != "User")
            session_->selectAutoMembers(autoId.toStdString());
    });
    connect(this, &QTreeWidget::customContextMenuRequested, this, [this](QPoint pos) {
        setCurrentItem(itemAt(pos));
        updateActions();
        QMenu menu(this);
        menu.addActions(actions());
        menu.exec(viewport()->mapToGlobal(pos));
    });
    connect(this, &QTreeWidget::currentItemChanged, this, [this] { updateActions(); });
    connect(this, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int) {
        if (!attached()) return;
        if (item->data(0, Qt::UserRole).isValid()) {
            const int id = item->data(0, Qt::UserRole).toInt();
            if (item->text(0) != item->data(0, nameRole).toString())
                session_->renameVisgroup(id, item->text(0).toStdString());
            else session_->setVisgroupShown(id, item->checkState(0) == Qt::Checked);
        } else if (item->flags().testFlag(Qt::ItemIsUserCheckable)) {
            session_->setAutoVisgroupShown(item->data(0, autoRole).toString().toStdString(), item->checkState(0) == Qt::Checked);
        }
        scheduleRefresh();
    });
    updateActions();
}

VisgroupPanel::~VisgroupPanel() {
    setEditorSession(nullptr);
    if (document_) document_->unobserve(documentObserver_);
}

bool VisgroupPanel::attached() const { return session_ && document_ && session_->document() == document_; }

void VisgroupPanel::scheduleRefresh() {
    if (refreshPending_) return;
    refreshPending_ = true;
    // Finish inline editing and drag delivery before replacing their tree items.
    QMetaObject::invokeMethod(this, [this] { if (refreshPending_) refresh(); }, Qt::QueuedConnection);
}

void VisgroupPanel::setDocument(document::Document* document) {
    if (document_) document_->unobserve(documentObserver_);
    document_ = document;
    if (document_) documentObserver_ = document_->observe([this](const document::ChangeSet&) { scheduleRefresh(); });
    refresh();
}

void VisgroupPanel::setEditorSession(editor::EditorSession* session) {
    if (session_) session_->unobserve(sessionObserver_);
    session_ = session;
    if (session_) sessionObserver_ = session_->observe([this] { updateActions(); scheduleRefresh(); });
    refresh();
}

std::optional<int> VisgroupPanel::currentVisgroup() const {
    if (currentItem() && currentItem()->data(0, Qt::UserRole).isValid()) return currentItem()->data(0, Qt::UserRole).toInt();
    return {};
}

void VisgroupPanel::updateActions() {
    const bool user = attached() && currentVisgroup().has_value();
    const bool selection = attached() && !editor::selectedNodes(session_->selection()).empty();
    for (auto* a : actions()) {
        const auto id = a->objectName();
        if (id == "visgroup.new") a->setEnabled(attached());
        else if (id == "visgroup.add" || id == "visgroup.remove") a->setEnabled(user && selection);
        else if (id == "visgroup.select") a->setEnabled(attached() && currentItem() &&
            (user || currentItem()->flags().testFlag(Qt::ItemIsUserCheckable)));
        else a->setEnabled(user);
    }
}

bool VisgroupPanel::newVisgroup(bool child) {
    if (!attached() || (child && !currentVisgroup())) return false;
    const auto result = session_->createVisgroup(tr("New visgroup").toStdString(), child ? currentVisgroup() : std::nullopt);
    if (!result) return false;
    refresh();
    for (QTreeWidgetItemIterator it(this); *it; ++it) {
        if ((*it)->data(0, Qt::UserRole).isValid() && (*it)->data(0, Qt::UserRole).toInt() == *result) {
            setCurrentItem(*it);
            for (auto* p = (*it)->parent(); p; p = p->parent()) p->setExpanded(true);
            editItem(*it);
            break;
        }
    }
    return true;
}

bool VisgroupPanel::moveVisgroup(int id, std::optional<int> parent) {
    return attached() && session_->moveVisgroup(id, parent).has_value();
}

void VisgroupPanel::startDrag(Qt::DropActions) {
    if (!attached() || !currentVisgroup()) return;
    auto* mime = new QMimeData;
    mime->setData(visgroupMime, QByteArray::number(*currentVisgroup()));
    QDrag drag(this);
    drag.setMimeData(mime);
    drag.exec(Qt::MoveAction);
}

void VisgroupPanel::dragEnterEvent(QDragEnterEvent* event) {
    if (attached() && event->mimeData()->hasFormat(visgroupMime) && (!event->source() || event->source() == this))
        event->acceptProposedAction();
    else event->ignore();
}

void VisgroupPanel::dragMoveEvent(QDragMoveEvent* event) {
    if (attached() && event->mimeData()->hasFormat(visgroupMime) && (!event->source() || event->source() == this))
        event->acceptProposedAction();
    else event->ignore();
}

void VisgroupPanel::dropEvent(QDropEvent* event) {
    if (!event->mimeData()->hasFormat(visgroupMime) || (event->source() && event->source() != this)) {
        event->ignore();
        return;
    }
    bool valid = false;
    const int id = event->mimeData()->data(visgroupMime).toInt(&valid);
    if (!valid) { event->ignore(); return; }
    auto* target = itemAt(event->position().toPoint());
    std::optional<int> parent;
    if (target && target->data(0, Qt::UserRole).isValid()) parent = target->data(0, Qt::UserRole).toInt();
    else if (target && target->data(0, autoRole).toString() != "User") { event->ignore(); return; }
    if (moveVisgroup(id, parent)) {
        if (target) target->setExpanded(true);
        event->setDropAction(Qt::MoveAction);
        event->accept();
    } else event->ignore();
}

void VisgroupPanel::refresh() {
    refreshPending_ = false;
    const QSignalBlocker blocker(this);
    const auto selected = identity(currentItem());
    std::map<QString, bool> expanded;
    for (QTreeWidgetItemIterator it(this); *it; ++it) expanded[identity(*it)] = (*it)->isExpanded();
    clear();
    if (document_) {
        auto* user = new QTreeWidgetItem(this, {tr("User")});
        user->setData(0, autoRole, "User");
        user->setFlags(Qt::ItemIsEnabled | Qt::ItemIsDropEnabled);
        const auto tree = editor::buildVisgroupTree(*document_);
        std::map<document::NodeId, QTreeWidgetItem*> items;
        for (const auto& node : tree.nodes) {
            auto* item = new QTreeWidgetItem;
            item->setText(0, text(node.name));
            item->setData(0, nameRole, text(node.name));
            item->setData(0, Qt::UserRole, node.vmfId);
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable |
                           Qt::ItemIsUserCheckable | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled);
            item->setCheckState(0, tree.shown(node.vmfId) ? Qt::Checked : Qt::Unchecked);
            std::istringstream color(node.color);
            int r, g, b;
            if (color >> r >> g >> b) {
                const QColor c(r, g, b);
                if (c.isValid()) { QPixmap swatch(12, 12); swatch.fill(c); item->setIcon(0, QIcon(swatch)); }
            }
            items[node.node] = item;
        }
        for (const auto& node : tree.nodes) {
            auto* parent = items.contains(node.parent) ? items.at(node.parent) : user;
            parent->addChild(items.at(node.node));
        }
        auto* automatic = new QTreeWidgetItem(this, {tr("Auto")});
        automatic->setData(0, autoRole, "Auto");
        automatic->setFlags(Qt::ItemIsEnabled);
        for (const auto& group : editor::autoVisgroups()) {
            auto* item = new QTreeWidgetItem(automatic, {text(group.name)});
            item->setData(0, autoRole, text(group.id));
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
            const bool shown = attached() ? session_->autoVisgroupShown(group.id) :
                !editor::buildVisgroupVisibility(*document_).automatic.contains(group.id);
            item->setCheckState(0, shown ? Qt::Checked : Qt::Unchecked);
        }
        for (QTreeWidgetItemIterator it(this); *it; ++it) {
            const auto key = identity(*it);
            (*it)->setExpanded(expanded.contains(key) ? expanded.at(key) : true);
            if (key == selected) setCurrentItem(*it);
        }
    }
    updateActions();
}

} // namespace apex::ui
