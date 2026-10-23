#include "ui/workspace_bar.hpp"

#include <QHBoxLayout>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QSettings>
#include <QTabBar>
#include <QToolButton>

#include "core/log.hpp"
#include "ui/viewport_area.hpp"

namespace apex::ui {
namespace {
QString fromStd(std::string_view s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }
}  // namespace

WorkspaceBar::WorkspaceBar(ViewportArea& area, QWidget* parent) : QWidget(parent), area_(area) {
    setObjectName(QStringLiteral("WorkspaceBar"));
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(4, 2, 4, 0);
    row->setSpacing(2);
    tabs_ = new QTabBar(this);
    tabs_->setObjectName(QStringLiteral("WorkspaceTabs"));
    tabs_->setMovable(true);
    tabs_->setExpanding(false);
    tabs_->setDrawBase(false);
    tabs_->setDocumentMode(true);
    tabs_->setContextMenuPolicy(Qt::CustomContextMenu);
    row->addWidget(tabs_);
    add_ = new QToolButton(this);
    add_->setObjectName(QStringLiteral("WorkspaceAdd"));
    add_->setText(QStringLiteral("+"));
    add_->setToolTip(tr("Add a workspace"));
    add_->setAutoRaise(true);
    add_->setPopupMode(QToolButton::InstantPopup);
    auto* menu = new QMenu(add_);
    for (const auto name : viewport::workspaceTemplateNames())
        menu->addAction(fromStd(name), this, [this, n = std::string(name)] { addFromTemplate(n); });
    menu->addSeparator();
    menu->addAction(tr("Duplicate current"), this, [this] { duplicate(set_.active); });
    add_->setMenu(menu);
    row->addWidget(add_);
    row->addStretch(1);

    connect(tabs_, &QTabBar::currentChanged, this, [this](int index) {
        if (!syncing_ && index >= 0) switchTo(static_cast<std::size_t>(index));
    });
    connect(tabs_, &QTabBar::tabMoved, this, [this](int from, int to) {
        if (syncing_) return;
        (void)set_.move(static_cast<std::size_t>(from), static_cast<std::size_t>(to));
    });
    connect(tabs_, &QTabBar::tabBarDoubleClicked, this, [this](int index) {
        if (index >= 0) promptRename(static_cast<std::size_t>(index));
    });
    connect(tabs_, &QWidget::customContextMenuRequested, this, &WorkspaceBar::showTabMenu);

    set_ = viewport::WorkspaceSet::defaults();
    syncTabs();
}

void WorkspaceBar::load() {
    const auto bytes = QSettings().value(QStringLiteral("ui/workspaces")).toByteArray();
    set_ = viewport::WorkspaceSet::defaults();
    if (!bytes.isEmpty()) {
        auto parsed = json::parse(std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())));
        if (!parsed) {
            log::warn("workspaces: saved set unreadable: {}", parsed.error().what());
        } else if (auto restored = viewport::WorkspaceSet::fromJson(*parsed)) {
            set_ = std::move(*restored);
        } else {
            log::warn("workspaces: saved set rejected: {}", restored.error().what());
        }
    }
    area_.setLayout(set_.workspaces[set_.active].layout);
    syncTabs();
    emit switched();
}

void WorkspaceBar::save() {
    capture();
    const std::string text = json::write(set_.toJson());
    QSettings().setValue(QStringLiteral("ui/workspaces"), QByteArray(text.data(), static_cast<qsizetype>(text.size())));
}

const viewport::WorkspaceSet& WorkspaceBar::workspaces() {
    capture();
    return set_;
}

void WorkspaceBar::capture() { set_.workspaces[set_.active].layout = area_.viewportLayout(); }

void WorkspaceBar::switchTo(std::size_t index) {
    if (index >= set_.workspaces.size()) return;
    if (index != set_.active) {
        capture();
        set_.active = index;
        area_.setLayout(set_.workspaces[index].layout);
    }
    syncTabs();
    emit switched();
}

void WorkspaceBar::step(int delta) {
    const auto n = static_cast<int>(set_.workspaces.size());
    switchTo(static_cast<std::size_t>(((static_cast<int>(set_.active) + delta) % n + n) % n));
}

void WorkspaceBar::addFromTemplate(std::string_view name) {
    capture();
    set_.workspaces.push_back({set_.uniqueName(name), viewport::workspaceTemplate(name), std::string(name)});
    switchTo(set_.workspaces.size() - 1);
}

void WorkspaceBar::duplicate(std::size_t index) {
    if (index >= set_.workspaces.size()) return;
    capture();
    auto copy = set_.workspaces[index];
    copy.name = set_.uniqueName(copy.name);
    set_.workspaces.insert(set_.workspaces.begin() + static_cast<std::ptrdiff_t>(index) + 1, std::move(copy));
    if (set_.active > index) ++set_.active;
    switchTo(index + 1);
}

void WorkspaceBar::rename(std::size_t index, const QString& name) {
    const auto trimmed = name.trimmed().toStdString();
    if (index >= set_.workspaces.size() || trimmed.empty() || trimmed == set_.workspaces[index].name) return;
    set_.workspaces[index].name = set_.uniqueName(trimmed);
    syncTabs();
}

void WorkspaceBar::remove(std::size_t index) {
    capture();
    const auto before = set_.active;
    if (!set_.remove(index)) return;
    if (index == before) area_.setLayout(set_.workspaces[set_.active].layout);
    syncTabs();
    emit switched();
}

void WorkspaceBar::resetActive() {
    auto& w = set_.workspaces[set_.active];
    w.layout = viewport::workspaceTemplate(w.base);
    area_.setLayout(w.layout);
    emit switched();
}

void WorkspaceBar::syncTabs() {
    syncing_ = true;
    while (tabs_->count() > static_cast<int>(set_.workspaces.size())) tabs_->removeTab(tabs_->count() - 1);
    for (std::size_t i = 0; i < set_.workspaces.size(); ++i) {
        const QString name = QString::fromStdString(set_.workspaces[i].name);
        if (static_cast<int>(i) < tabs_->count()) tabs_->setTabText(static_cast<int>(i), name);
        else tabs_->addTab(name);
    }
    tabs_->setCurrentIndex(static_cast<int>(set_.active));
    syncing_ = false;
}

void WorkspaceBar::showTabMenu(const QPoint& pos) {
    const int tab = tabs_->tabAt(pos);
    if (tab < 0) return;
    const auto index = static_cast<std::size_t>(tab);
    QMenu menu(this);
    menu.addAction(tr("Rename..."), this, [this, index] { promptRename(index); });
    menu.addAction(tr("Duplicate"), this, [this, index] { duplicate(index); });
    auto* reset = menu.addAction(tr("Reset to \"%1\"").arg(QString::fromStdString(set_.workspaces[index].base)), this,
                                 [this, index] {
                                     switchTo(index);
                                     resetActive();
                                 });
    reset->setToolTip(tr("Replace this workspace's areas with its template"));
    menu.addSeparator();
    auto* del = menu.addAction(tr("Delete"), this, [this, index] {
        const auto answer = QMessageBox::question(
            this, tr("Delete workspace"),
            tr("Delete the workspace \"%1\"?").arg(QString::fromStdString(set_.workspaces[index].name)));
        if (answer == QMessageBox::Yes) remove(index);
    });
    del->setEnabled(set_.workspaces.size() > 1);
    menu.exec(tabs_->mapToGlobal(pos));
}

void WorkspaceBar::promptRename(std::size_t index) {
    if (index >= set_.workspaces.size()) return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Rename workspace"), tr("Name:"), QLineEdit::Normal,
                                               QString::fromStdString(set_.workspaces[index].name), &ok);
    if (ok) rename(index, name);
}

}  // namespace apex::ui
