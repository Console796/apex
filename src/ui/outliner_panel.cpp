#include "ui/outliner_panel.hpp"

#include <QString>
#include <QLineEdit>
#include <QStyledItemDelegate>
#include <QDrag>
#include <QMimeData>
#include <QDropEvent>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QStyle>
#include <QTimer>
#include "editor/visibility.hpp"
#include <QApplication>
#include <QSignalBlocker>
#include <QTreeWidgetItemIterator>
#include <string>
#include <algorithm>
#include <string_view>

#include "document/document.hpp"

namespace apex::ui {
namespace {

QString text(std::string_view s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }

QString value(const kv1::Node& block, std::string_view key) {
    if (const auto* n = block.find(key)) {
        if (auto v = n->decodedValue()) return text(*v);
    }
    return {};
}

bool is(const document::Node& n, std::string_view key) {
    if (!n.block) return false;
    const auto k = n.block->key();
    if (k.size() != key.size()) return false;
    for (std::size_t i = 0; i < k.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(k[i])) != key[i]) return false;
    }
    return true;
}

QString entityLabel(const document::Node& n) {
    const QString cls = value(*n.block, "classname");
    const QString name = value(*n.block, "targetname");
    QString label = cls.isEmpty() ? QStringLiteral("entity") : cls;
    if (!name.isEmpty()) label += QStringLiteral("  \"%1\"").arg(name);
    return label + QStringLiteral("  #%1").arg(value(*n.block, "id"));
}

constexpr int NameRole = Qt::UserRole + 1;
constexpr int EntityRole = Qt::UserRole + 2;
constexpr int ExpansionRole = Qt::UserRole + 3;
const char* const mimeType = "application/x-apex-outliner";

class NameDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    editor::EditorSession* session = nullptr;
    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option,
                          const QModelIndex& index) const override {
        if (!index.data(EntityRole).toBool()) return nullptr;
        return QStyledItemDelegate::createEditor(parent, option, index);
    }
    void setEditorData(QWidget* editor, const QModelIndex& index) const override {
        static_cast<QLineEdit*>(editor)->setText(index.data(NameRole).toString());
    }
    void setModelData(QWidget* editor, QAbstractItemModel*, const QModelIndex& index) const override {
        const auto id = index.data(Qt::UserRole).toULongLong();
        const auto name = static_cast<QLineEdit*>(editor)->text().toStdString();
        const auto* document = session ? session->document() : nullptr;
        // Document notifications rebuild the tree; let Qt finish closing the editor first.
        QTimer::singleShot(0, this, [this, id, name, document] {
            if (session && session->document() == document) session->renameEntity(id, name);
        });
    }
};

QTreeWidgetItem* section(QTreeWidget* tree, const QString& title, int count) {
    auto* item = new QTreeWidgetItem(tree, {QStringLiteral("%1 (%2)").arg(title).arg(count)});
    QFont f = item->font(0);
    f.setBold(true);
    item->setFont(0, f);
    return item;
}

}  // namespace

OutlinerPanel::OutlinerPanel(QWidget* parent) : QTreeWidget(parent) {
    setHeaderHidden(false);
    setEditTriggers(DoubleClicked | EditKeyPressed);
    auto* delegate = new NameDelegate(this);
    setItemDelegate(delegate);
    setDragEnabled(true);
    setAcceptDrops(true);
    setDropIndicatorShown(true);
    setDragDropMode(DragDrop);
    filter_ = new QLineEdit(header());
    filter_->setObjectName("outlinerFilter");
    filter_->setPlaceholderText(tr("Filter objects"));
    auto* filterLayout = new QHBoxLayout(header());
    filterLayout->setContentsMargins(0, 0, 0, 0);
    filterLayout->addWidget(filter_);
    header()->setFixedHeight(filter_->sizeHint().height());
    connect(filter_, &QLineEdit::textChanged, this, &OutlinerPanel::applyFilter);
    connect(this, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int) {
        const auto id = item->data(0, Qt::UserRole).toULongLong();
        if (!session_ || !id) return;
        const bool hidden = item->checkState(0) != Qt::Checked;
        const auto* document = document_;
        // Rebuilding during itemChanged would invalidate Qt's model signal arguments.
        QTimer::singleShot(0, this, [this, id, hidden, document] {
            if (!session_ || document_ != document || session_->document() != document) return;
            session_->setNodeHidden({id}, hidden);
            refresh();
        });
    });
    setSelectionMode(ExtendedSelection);
    setUniformRowHeights(true);
    setAlternatingRowColors(false);
    connect(this, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int) {
        const auto id = item->data(0, Qt::UserRole).toULongLong();
        if (!session_ || !id || !item->flags().testFlag(Qt::ItemIsSelectable)) return;
        const auto modifiers = QApplication::keyboardModifiers();
        session_->selectNode(id, {modifiers.testFlag(Qt::ControlModifier), modifiers.testFlag(Qt::ShiftModifier)});
        syncSelection();
    });
}

OutlinerPanel::~OutlinerPanel() {
    setEditorSession(nullptr);
    if (document_) document_->unobserve(documentObserver_);
}

void OutlinerPanel::setEditorSession(editor::EditorSession* session) {
    if (session_) session_->unobserve(sessionObserver_);
    session_ = session;
    static_cast<NameDelegate*>(itemDelegate())->session = session;
    if (session_) sessionObserver_ = session_->observe([this] {
        if (syncedSelection_ != session_->selection().changed()) syncSelection();
    });
    syncSelection();
}

void OutlinerPanel::syncSelection() {
    const QSignalBlocker blocker(this);
    syncedSelection_ = session_ ? std::optional(session_->selection().changed()) : std::nullopt;
    clearSelection();
    if (!session_ || session_->document() != document_) return;
    std::set<document::NodeId> ids;
    std::optional<document::NodeId> active;
    const auto nodeOf = [&](editor::Target target) -> std::optional<document::NodeId> {
        if (const auto* id = std::get_if<document::NodeId>(&target)) return *id;
        if (const auto location = document_->side(std::get<document::SideId>(target)))
            return document_->get(location->solid)->id;
        return std::nullopt;
    };
    for (const auto target : session_->selection().ordered()) {
        if (const auto id = nodeOf(target)) ids.insert(*id);
    }
    if (const auto target = session_->selection().active()) active = nodeOf(*target);
    for (QTreeWidgetItemIterator it(this); *it; ++it) {
        const auto id = (*it)->data(0, Qt::UserRole).toULongLong();
        (*it)->setSelected(ids.contains(id));
        if (id && active == id) scrollToItem(*it);
    }
}

void OutlinerPanel::setDocument(document::Document* document) {
    if (document_) document_->unobserve(documentObserver_);
    document_ = document;
    if (document_) documentObserver_ = document_->observe([this](const document::ChangeSet&) { refresh(); });
    refresh();
}

void OutlinerPanel::refresh() {
    const QSignalBlocker blocker(this);
    std::map<QString, bool> expanded;
    for (QTreeWidgetItemIterator it(this); *it; ++it)
        expanded[(*it)->data(0, ExpansionRole).toString()] = (*it)->isExpanded();
    clear();
    if (!document_) return;
    const auto& doc = *document_;
    editor::GroupIndex groups(doc);
    const auto node = [&](document::NodeId id) -> const document::Node* {
        const auto h = doc.handle(id);
        return h ? doc.get(*h) : nullptr;
    };
    std::vector<const document::Node*> objects, visgroups;
    const document::Node* worldNode = nullptr;
    const auto collect = [&](const auto& self, document::NodeId id) -> void {
        const auto* n = node(id);
        if (!n) return;
        if (is(*n, "world")) worldNode = n;
        else if (is(*n, "entity") || is(*n, "solid") || is(*n, "group")) objects.push_back(n);
        if (is(*n, "visgroups")) {
            for (const auto child : n->children) if (const auto* v = node(child)) visgroups.push_back(v);
        }
        for (const auto child : n->children) self(self, child);
    };
    for (const auto root : doc.roots()) collect(collect, root);
    QTreeWidgetItem* world = nullptr;
    if (worldNode) {
        world = section(this, tr("World"), static_cast<int>(editor::solidsOf(doc, worldNode->id).size()));
        world->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(worldNode->id));
        world->setFlags(Qt::ItemIsEnabled | Qt::ItemIsDropEnabled);
    }
    const auto count = [&](std::string_view key) {
        return static_cast<int>(std::ranges::count_if(objects, [&](const auto* n) { return is(*n, key); }));
    };
    auto* ents = section(this, tr("Entities"), count("entity"));
    auto* grp = section(this, tr("Groups"), count("group"));
    std::map<document::NodeId, QTreeWidgetItem*> rows;
    if (worldNode) rows[worldNode->id] = world;
    for (const auto* n : objects) {
        QString label = is(*n, "entity") ? entityLabel(*n) :
            tr("%1 #%2").arg(is(*n, "solid") ? tr("Solid") : tr("Group"), value(*n->block, "id"));
        auto* item = new QTreeWidgetItem({label});
        rows[n->id] = item;
        item->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(n->id));
        item->setData(0, ExpansionRole, QString::number(n->id));
        auto flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled | Qt::ItemIsUserCheckable;
        if (!is(*n, "solid")) flags |= Qt::ItemIsDropEnabled;
        if (is(*n, "entity")) {
            flags |= Qt::ItemIsEditable;
            item->setData(0, EntityRole, true);
            item->setData(0, NameRole, value(*n->block, "targetname"));
        }
        item->setFlags(flags);
        bool hidden = !editor::isVisible(doc, n->id);
        if (is(*n, "group")) {
            const auto members = groups.members(n->id, true);
            hidden = hidden || (!members.empty() && std::ranges::all_of(members, [&](auto id) {
                return !editor::isVisible(doc, id) || is(*node(id), "group");
            }));
        }
        item->setCheckState(0, hidden ? Qt::Unchecked : Qt::Checked);
        if (hidden) item->setForeground(0, palette().brush(QPalette::Disabled, QPalette::Text));
    }
    for (const auto* n : objects) {
        QTreeWidgetItem* parent = nullptr;
        if (is(*n, "solid")) {
            if (const auto owner = editor::brushEntityOf(doc, n->id)) parent = rows[*owner];
        }
        if (!parent) if (const auto group = groups.parentGroup(n->id)) parent = rows[*group];
        if (!parent) parent = is(*n, "group") ? grp : is(*n, "entity") ? ents : world;
        if (parent) parent->addChild(rows[n->id]);
        else addTopLevelItem(rows[n->id]);
    }

    auto* vis = section(this, tr("Visgroups"), static_cast<int>(visgroups.size()));
    const auto addVis = [&](auto&& self, QTreeWidgetItem* parent, const document::Node* v) -> void {
        if (!v->block || !is(*v, "visgroup")) return;
        QString name = value(*v->block, "name");
        if (name.isEmpty()) name = tr("(unnamed)");
        auto* item = new QTreeWidgetItem(parent, {name});
        for (const auto childId : v->children) {
            if (const auto* c = node(childId)) self(self, item, c);
        }
    };
    for (const auto* v : visgroups) addVis(addVis, vis, v);

    for (const auto& entry : {std::pair{world, QStringLiteral("world")}, {ents, QStringLiteral("entities")},
                              {grp, QStringLiteral("groups")}, {vis, QStringLiteral("visgroups")}}) {
        if (!entry.first) continue;
        entry.first->setData(0, ExpansionRole, entry.second);
        if (entry.first != world) entry.first->setFlags(Qt::ItemIsEnabled);
    }
    for (QTreeWidgetItemIterator it(this); *it; ++it) {
        const auto key = (*it)->data(0, ExpansionRole).toString();
        (*it)->setExpanded(expanded.contains(key) ? expanded[key] : *it != world);
    }
    applyFilter();
    syncSelection();
}

void OutlinerPanel::applyFilter() {
    const auto filter = filter_->text();
    const auto visit = [&](const auto& self, QTreeWidgetItem* item) -> bool {
        bool match = item->text(0).contains(filter, Qt::CaseInsensitive);
        for (int i = 0; i < item->childCount(); ++i) match = self(self, item->child(i)) || match;
        item->setHidden(!match);
        return match;
    };
    for (int i = 0; i < topLevelItemCount(); ++i) visit(visit, topLevelItem(i));
}
void OutlinerPanel::mousePressEvent(QMouseEvent* event) {
    visibilityClick_ = false;
    auto* item = itemAt(event->position().toPoint());
    if (event->button() == Qt::LeftButton && item && item->flags().testFlag(Qt::ItemIsUserCheckable)) {
        QStyleOptionViewItem option;
        initViewItemOption(&option);
        option.rect = visualItemRect(item);
        option.features |= QStyleOptionViewItem::HasCheckIndicator;
        const auto check = style()->subElementRect(QStyle::SE_ItemViewItemCheckIndicator, &option, this);
        if (check.contains(event->position().toPoint())) {
            visibilityClick_ = true;
            item->setCheckState(0, item->checkState(0) == Qt::Checked ? Qt::Unchecked : Qt::Checked);
            event->accept();
            return;
        }
    }
    QTreeWidget::mousePressEvent(event);
}
void OutlinerPanel::mouseReleaseEvent(QMouseEvent* event) {
    if (visibilityClick_) {
        visibilityClick_ = false;
        event->accept();
        return;
    }
    QTreeWidget::mouseReleaseEvent(event);
}
QMimeData* OutlinerPanel::mimeData(const QList<QTreeWidgetItem*>& items) const {
    auto* mime = new QMimeData;
    QByteArray payload;
    for (const auto* item : items) {
        const auto id = item->data(0, Qt::UserRole).toULongLong();
        if (id) payload += QByteArray::number(id) + '\n';
    }
    mime->setData(mimeType, payload);
    mime->setProperty("outlinerSource", QVariant::fromValue(static_cast<const QObject*>(this)));
    return mime;
}
bool OutlinerPanel::dropMimeData(QTreeWidgetItem* parent, int, const QMimeData* mime, Qt::DropAction action) {
    if (!session_ || !parent || action != Qt::MoveAction || !mime->hasFormat(mimeType) ||
        mime->property("outlinerSource").value<const QObject*>() != this ||
        !parent->flags().testFlag(Qt::ItemIsDropEnabled)) return false;
    std::vector<document::NodeId> nodes;
    for (const auto& part : mime->data(mimeType).split('\n')) {
        if (part.isEmpty()) continue;
        bool ok = false;
        const auto id = part.toULongLong(&ok);
        if (!ok || !id) return false;
        nodes.push_back(id);
    }
    return session_->moveNodesInto(std::move(nodes), parent->data(0, Qt::UserRole).toULongLong()).has_value();
}
void OutlinerPanel::startDrag(Qt::DropActions) {
    QDrag drag(this);
    drag.setMimeData(mimeData(selectedItems()));
    drag.exec(Qt::MoveAction);
}
void OutlinerPanel::dragEnterEvent(QDragEnterEvent* event) {
    if (event->source() == this && event->mimeData()->hasFormat(mimeType)) event->acceptProposedAction();
    else event->ignore();
}
void OutlinerPanel::dragMoveEvent(QDragMoveEvent* event) {
    const auto* item = itemAt(event->position().toPoint());
    if (event->source() == this && item && item->flags().testFlag(Qt::ItemIsDropEnabled))
        event->acceptProposedAction();
    else event->ignore();
}
void OutlinerPanel::dropEvent(QDropEvent* event) {
    auto* item = itemAt(event->position().toPoint());
    if (!session_ || event->source() != this || !item ||
        !event->mimeData()->hasFormat(mimeType) || !item->flags().testFlag(Qt::ItemIsDropEnabled)) {
        event->ignore();
        return;
    }
    if (dropMimeData(item, -1, event->mimeData(), Qt::MoveAction))
        event->acceptProposedAction();
    else event->ignore();
}

}  // namespace apex::ui
