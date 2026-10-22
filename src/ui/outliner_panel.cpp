#include "ui/outliner_panel.hpp"

#include <QString>
#include <QApplication>
#include <QSignalBlocker>
#include <QTreeWidgetItemIterator>
#include <string>
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

QTreeWidgetItem* section(QTreeWidget* tree, const QString& title, int count) {
    auto* item = new QTreeWidgetItem(tree, {QStringLiteral("%1 (%2)").arg(title).arg(count)});
    QFont f = item->font(0);
    f.setBold(true);
    item->setFont(0, f);
    return item;
}

}  // namespace

OutlinerPanel::OutlinerPanel(QWidget* parent) : QTreeWidget(parent) {
    setHeaderHidden(true);
    setEditTriggers(NoEditTriggers);
    setSelectionMode(ExtendedSelection);
    setUniformRowHeights(true);
    setAlternatingRowColors(false);
    connect(this, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int) {
        const auto id = item->data(0, Qt::UserRole).toULongLong();
        if (!session_ || !id) return;
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
    clear();
    if (!document_) return;
    const auto& doc = *document_;
    const auto node = [&](document::NodeId id) -> const document::Node* {
        const auto h = doc.handle(id);
        return h ? doc.get(*h) : nullptr;
    };

    QTreeWidgetItem* world = nullptr;
    std::vector<const document::Node*> entities, groups, visgroups;
    for (const auto rootId : doc.roots()) {
        const auto* root = node(rootId);
        if (!root || !root->block) continue;
        if (is(*root, "world")) {
            world = new QTreeWidgetItem(this, {tr("World")});
            QFont f = world->font(0);
            f.setBold(true);
            world->setFont(0, f);
            int solids = 0;
            for (const auto childId : root->children) {
                const auto* c = node(childId);
                if (!c || !c->block) continue;
                if (is(*c, "solid")) ++solids;
                else if (is(*c, "group")) groups.push_back(c);
            }
            world->setText(0, tr("World (%1 solids)").arg(solids));
            const QString cls = value(*root->block, "classname");
            if (!cls.isEmpty()) new QTreeWidgetItem(world, {cls});
        } else if (is(*root, "entity")) {
            entities.push_back(root);
        } else if (is(*root, "group")) {
            groups.push_back(root);
        } else if (is(*root, "visgroups")) {
            for (const auto childId : root->children) {
                if (const auto* c = node(childId)) visgroups.push_back(c);
            }
        } else if (is(*root, "hidden")) {
            for (const auto childId : root->children) {
                if (const auto* c = node(childId); c && is(*c, "entity")) entities.push_back(c);
            }
        }
    }

    const auto addSolids = [&](const auto& self, QTreeWidgetItem* parent, const document::Node* n) -> void {
        for (const auto child : n->children) {
            const auto* c = node(child);
            if (!c) continue;
            if (is(*c, "solid")) {
                auto* item = new QTreeWidgetItem(parent, {tr("Solid #%1").arg(value(*c->block, "id"))});
                item->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(c->id));
            } else if (!is(*c, "group")) self(self, parent, c);
        }
    };
    if (world) {
        for (const auto root : doc.roots()) {
            if (const auto* n = node(root); n && is(*n, "world")) addSolids(addSolids, world, n);
        }
    }
    auto* ents = section(this, tr("Entities"), static_cast<int>(entities.size()));
    for (const auto* e : entities) {
        auto* item = new QTreeWidgetItem(ents, {entityLabel(*e)});
        item->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(e->id));
        addSolids(addSolids, item, e);
    }

    auto* grp = section(this, tr("Groups"), static_cast<int>(groups.size()));
    for (const auto* g : groups) {
        auto* item = new QTreeWidgetItem(grp, {tr("Group #%1").arg(value(*g->block, "id"))});
        item->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(g->id));
        int solids = 0, ents2 = 0;
        for (const auto childId : g->children) {
            const auto* c = node(childId);
            if (!c || !c->block) continue;
            if (is(*c, "solid")) ++solids;
            else if (is(*c, "entity")) ++ents2;
        }
        item->setText(0, tr("Group #%1  (%2 solids, %3 entities)")
                             .arg(value(*g->block, "id")).arg(solids).arg(ents2));
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

    if (world) world->setExpanded(false);
    ents->setExpanded(true);
    grp->setExpanded(true);
    vis->setExpanded(true);
    syncSelection();
}

}  // namespace apex::ui
