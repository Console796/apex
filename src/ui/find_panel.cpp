#include "ui/find_panel.hpp"

#include "editor/instance_templates.hpp"
#include "ui/game_content.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace apex::ui {
namespace {
QString text(std::string_view value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }
QString assetKind(editor::AssetKind kind) {
    switch (kind) {
    case editor::AssetKind::Material: return QObject::tr("Materials");
    case editor::AssetKind::Model: return QObject::tr("Models");
    case editor::AssetKind::Sound: return QObject::tr("Sounds");
    case editor::AssetKind::Particle: return QObject::tr("Particles");
    case editor::AssetKind::Instance: return QObject::tr("Instances");
    }
    return {};
}
void setupPreview(QTableWidget* table) {
    table->setColumnCount(3);
    table->setHorizontalHeaderLabels({QObject::tr("Object / Key"), QObject::tr("Before"), QObject::tr("After")});
    table->horizontalHeader()->setStretchLastSection(true);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
}
void addPreviewRow(QTableWidget* table, int row, QString object, QString before, QString after) {
    table->insertRow(row);
    table->setItem(row, 0, new QTableWidgetItem(std::move(object)));
    table->setItem(row, 1, new QTableWidgetItem(std::move(before)));
    table->setItem(row, 2, new QTableWidgetItem(std::move(after)));
}
}
FindPanel::FindPanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("FindPanel"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(6, 6, 6, 6);
    modes_ = new QTabWidget(this);
    modes_->setObjectName(QStringLiteral("findModes"));
    root->addWidget(modes_);

    auto* report = new QWidget(modes_);
    auto* reportLayout = new QVBoxLayout(report);
    auto* filters = new QWidget(report);
    auto* form = new QFormLayout(filters);
    class_ = new QLineEdit(filters); class_->setObjectName(QStringLiteral("findClass"));
    name_ = new QLineEdit(filters); name_->setObjectName(QStringLiteral("findName"));
    keyContains_ = new QLineEdit(filters); keyContains_->setObjectName(QStringLiteral("findKeyContains"));
    visgroup_ = new QComboBox(filters); visgroup_->setObjectName(QStringLiteral("findVisgroup"));
    inside_ = new QCheckBox(tr("Inside selection"), filters); inside_->setObjectName(QStringLiteral("findInsideSelection"));
    brush_ = new QComboBox(filters); brush_->setObjectName(QStringLiteral("findBrushPoint"));
    brush_->addItems({tr("Any"), tr("Brush"), tr("Point")});
    form->addRow(tr("Class (* wildcard)"), class_);
    form->addRow(tr("Name contains"), name_);
    form->addRow(tr("Key or value contains"), keyContains_);
    form->addRow(tr("Visgroup"), visgroup_);
    form->addRow(inside_);
    form->addRow(tr("Entity kind"), brush_);
    reportLayout->addWidget(filters);
    auto* actions = new QHBoxLayout;
    auto* refreshButton = new QPushButton(tr("Refresh"), report); refreshButton->setObjectName(QStringLiteral("findRefresh"));
    auto* frame = new QPushButton(tr("Frame"), report); frame->setObjectName(QStringLiteral("findFrame"));
    actions->addWidget(refreshButton); actions->addWidget(frame); actions->addStretch();
    reportLayout->addLayout(actions);
    auto* views = new QTabWidget(report);
    entities_ = new QTableWidget(views); entities_->setObjectName(QStringLiteral("findEntities"));
    entities_->setColumnCount(3); entities_->setHorizontalHeaderLabels({tr("Class"), tr("Name"), tr("ID")});
    entities_->horizontalHeader()->setStretchLastSection(true);
    entities_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    entities_->setSelectionBehavior(QAbstractItemView::SelectRows);
    entities_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    problems_ = new QTreeWidget(views); problems_->setObjectName(QStringLiteral("findProblems"));
    problems_->setHeaderLabels({tr("Problem"), tr("Details")});
    addons_ = new QTreeWidget(views); addons_->setObjectName(QStringLiteral("findAddons"));
    addons_->setHeaderLabels({tr("Addon"), tr("Referenced assets")});
    views->addTab(entities_, tr("Entities")); views->addTab(problems_, tr("Problems")); views->addTab(addons_, tr("Addons this map uses"));
    reportLayout->addWidget(views, 1);
    modes_->addTab(report, tr("Report"));

    auto* replace = new QWidget(modes_);
    auto* replaceLayout = new QVBoxLayout(replace);
    auto* keyBox = new QGroupBox(tr("Keyvalues"), replace);
    auto* keyLayout = new QVBoxLayout(keyBox);
    auto* keyFormWidget = new QWidget(keyBox); auto* keyForm = new QFormLayout(keyFormWidget);
    keyFind_ = new QLineEdit(keyFormWidget); keyFind_->setObjectName(QStringLiteral("keyReplaceFind"));
    keyReplace_ = new QLineEdit(keyFormWidget); keyReplace_->setObjectName(QStringLiteral("keyReplaceWith"));
    keyForm->addRow(tr("Find"), keyFind_); keyForm->addRow(tr("Replace with"), keyReplace_);
    keyLayout->addWidget(keyFormWidget);
    auto* keyFlags = new QHBoxLayout;
    wholeWord_ = new QCheckBox(tr("Whole word"), keyBox); caseSensitive_ = new QCheckBox(tr("Case sensitive"), keyBox);
    wildcard_ = new QCheckBox(tr("* wildcard"), keyBox); connections_ = new QCheckBox(tr("Connection fields"), keyBox);
    wholeWord_->setObjectName("keyReplaceWholeWord"); caseSensitive_->setObjectName("keyReplaceCase");
    wildcard_->setObjectName("keyReplaceWildcard"); connections_->setObjectName("keyReplaceConnections");
    for (auto* box : {wholeWord_, caseSensitive_, wildcard_, connections_}) keyFlags->addWidget(box);
    keyLayout->addLayout(keyFlags);
    keyPreview_ = new QPushButton(tr("Preview"), keyBox); keyPreview_->setObjectName("keyReplacePreview");
    keyApply_ = new QPushButton(tr("Apply"), keyBox); keyApply_->setObjectName("keyReplaceApply"); keyApply_->setEnabled(false);
    auto* keyButtons = new QHBoxLayout; keyButtons->addWidget(keyPreview_); keyButtons->addWidget(keyApply_); keyButtons->addStretch();
    keyLayout->addLayout(keyButtons);
    keyChanges_ = new QTableWidget(keyBox); keyChanges_->setObjectName("keyReplaceChanges"); setupPreview(keyChanges_); keyLayout->addWidget(keyChanges_);
    replaceLayout->addWidget(keyBox, 1);

    auto* materialBox = new QGroupBox(tr("Materials (Replace Textures)"), replace);
    auto* materialLayout = new QVBoxLayout(materialBox);
    auto* materialFormWidget = new QWidget(materialBox); auto* materialForm = new QFormLayout(materialFormWidget);
    materialFind_ = new QLineEdit(materialFormWidget); materialFind_->setObjectName("materialReplaceFind");
    materialReplace_ = new QLineEdit(materialFormWidget); materialReplace_->setObjectName("materialReplaceWith");
    wholeName_ = new QCheckBox(tr("Whole material name"), materialBox); wholeName_->setObjectName("materialReplaceWholeName"); wholeName_->setChecked(true);
    materialKeys_ = new QCheckBox(tr("Also in material-typed entity keys"), materialBox); materialKeys_->setObjectName("materialReplaceKeys");
    materialScope_ = new QComboBox(materialBox); materialScope_->setObjectName("materialReplaceScope");
    materialScope_->addItems({tr("Selection"), tr("Whole map")});
    materialForm->addRow(tr("Find"), materialFind_); materialForm->addRow(tr("Replace with"), materialReplace_);
    materialForm->addRow(wholeName_); materialForm->addRow(materialKeys_); materialForm->addRow(tr("Scope"), materialScope_);
    materialLayout->addWidget(materialFormWidget);
    materialPreview_ = new QPushButton(tr("Preview"), materialBox); materialPreview_->setObjectName("materialReplacePreview");
    materialApply_ = new QPushButton(tr("Apply"), materialBox); materialApply_->setObjectName("materialReplaceApply"); materialApply_->setEnabled(false);
    auto* materialButtons = new QHBoxLayout; materialButtons->addWidget(materialPreview_); materialButtons->addWidget(materialApply_); materialButtons->addStretch();
    materialLayout->addLayout(materialButtons);
    materialChanges_ = new QTableWidget(materialBox); materialChanges_->setObjectName("materialReplaceChanges"); setupPreview(materialChanges_); materialLayout->addWidget(materialChanges_);
    replaceLayout->addWidget(materialBox, 1);
    modes_->addTab(replace, tr("Replace"));

    connect(refreshButton, &QPushButton::clicked, this, &FindPanel::refresh);
    connect(frame, &QPushButton::clicked, this, &FindPanel::frameRequested);
    const auto filterChanged = [this] { refreshEntities(); invalidateKeyPreview(); };
    for (auto* edit : {class_, name_, keyContains_}) connect(edit, &QLineEdit::textChanged, this, filterChanged);
    connect(visgroup_, &QComboBox::currentIndexChanged, this, filterChanged);
    connect(inside_, &QCheckBox::toggled, this, filterChanged);
    connect(brush_, &QComboBox::currentIndexChanged, this, filterChanged);
    connect(entities_, &QTableWidget::itemSelectionChanged, this, [this] {
        if (!session_ || !document_) return;
        std::set<document::NodeId> ids;
        for (const auto* item : entities_->selectedItems()) if (item->column() == 0)
            ids.insert(item->data(Qt::UserRole).toULongLong());
        session_->selection().setMode(editor::SelectionMode::Objects);
        std::vector<editor::Target> targets; for (auto id : ids) targets.emplace_back(id);
        session_->selection().replace(targets);
    });
    for (auto* edit : {keyFind_, keyReplace_}) connect(edit, &QLineEdit::textChanged, this, &FindPanel::invalidateKeyPreview);
    for (auto* box : {wholeWord_, caseSensitive_, wildcard_, connections_}) connect(box, &QCheckBox::toggled, this, &FindPanel::invalidateKeyPreview);
    connect(keyPreview_, &QPushButton::clicked, this, [this] {
        if (!document_) return;
        editor::KeyvalueReplaceOptions options;
        options.find = keyFind_->text().toStdString(); options.replacement = keyReplace_->text().toStdString();
        options.wholeWord = wholeWord_->isChecked(); options.caseSensitive = caseSensitive_->isChecked();
        options.wildcard = wildcard_->isChecked(); options.connections = connections_->isChecked();
        const auto rows = editor::entityReport(*session_, filter());
        if (!session_) return;
        auto result = editor::previewKeyvalueReplace(*document_, editor::entityReportIds(rows), options);
        keyChanges_->setRowCount(0); keyPreviewRows_.clear();
        if (!result) { keyApply_->setEnabled(false); return; }
        keyPreviewRows_ = std::move(*result);
        for (std::size_t i = 0; i < keyPreviewRows_.size(); ++i) {
            const auto& row = keyPreviewRows_[i];
            auto label = QStringLiteral("%1 #%2 · %3").arg(row.entity).arg(row.index).arg(text(row.key));
            if (row.newKey != row.key) label += QStringLiteral(" → %1").arg(text(row.newKey));
            addPreviewRow(keyChanges_, static_cast<int>(i), std::move(label), text(row.oldValue), text(row.newValue));
        }
        keyApply_->setEnabled(!keyPreviewRows_.empty());
    });
    connect(keyApply_, &QPushButton::clicked, this, [this] {
        if (!session_ || keyPreviewRows_.empty()) return;
        if (editor::applyKeyvalueReplace(*session_, keyPreviewRows_)) { keyPreviewRows_.clear(); keyApply_->setEnabled(false); refresh(); }
    });
    for (auto* edit : {materialFind_, materialReplace_}) connect(edit, &QLineEdit::textChanged, this, &FindPanel::invalidateMaterialPreview);
    connect(wholeName_, &QCheckBox::toggled, this, &FindPanel::invalidateMaterialPreview);
    connect(materialKeys_, &QCheckBox::toggled, this, &FindPanel::invalidateMaterialPreview);
    connect(materialScope_, &QComboBox::currentIndexChanged, this, &FindPanel::invalidateMaterialPreview);
    connect(materialPreview_, &QPushButton::clicked, this, [this] {
        if (!session_) return;
        editor::MaterialReplaceOptions options;
        options.find = materialFind_->text().toStdString(); options.replacement = materialReplace_->text().toStdString();
        options.wholeName = wholeName_->isChecked(); options.entityKeys = materialKeys_->isChecked();
        options.scope = materialScope_->currentIndex() == 0 ? editor::MaterialReplaceScope::Selection : editor::MaterialReplaceScope::WholeMap;
        auto result = editor::previewMaterialReplace(*session_, options);
        materialChanges_->setRowCount(0); materialPreviewRows_.clear();
        if (!result) { materialApply_->setEnabled(false); return; }
        materialPreviewRows_ = std::move(*result);
        for (std::size_t i = 0; i < materialPreviewRows_.size(); ++i) {
            const auto& row = materialPreviewRows_[i];
            const auto object = row.side ? QStringLiteral("solid %1 · side %2").arg(row.object).arg(*row.side)
                                         : QStringLiteral("entity %1 · %2").arg(row.object).arg(text(row.key));
            addPreviewRow(materialChanges_, static_cast<int>(i), object, text(row.oldValue), text(row.newValue));
        }
        materialApply_->setEnabled(!materialPreviewRows_.empty());
    });
    connect(materialApply_, &QPushButton::clicked, this, [this] {
        if (!session_ || materialPreviewRows_.empty()) return;
        if (editor::applyMaterialReplace(*session_, materialPreviewRows_)) { materialPreviewRows_.clear(); materialApply_->setEnabled(false); refresh(); }
    });
    connect(problems_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int) {
        const auto entity = item->data(0, Qt::UserRole).toULongLong();
        const auto solid = item->data(0, Qt::UserRole + 1).toULongLong();
        if (entity) selectEntities({entity}); else if (solid) selectEntities({solid});
    });
    refresh();
}
FindPanel::~FindPanel() { setEditorSession(nullptr); setDocument(nullptr); }
void FindPanel::setDocument(document::Document* doc) {
    if (document_) document_->unobserve(documentObserver_);
    document_ = doc;
    if (document_) documentObserver_ = document_->observe([this](const document::ChangeSet&) {
        invalidateKeyPreview(); invalidateMaterialPreview(); refresh();
    });
    refresh();
}
void FindPanel::setEditorSession(editor::EditorSession* session) {
    if (session_ && selectionObserver_) session_->selection().unobserve(selectionObserver_);
    session_ = session;
    selectionObserver_ = session_ ? session_->selection().observe([this](const editor::Selection&) {
        refreshEntities();
        invalidateKeyPreview();
        if (materialScope_ && materialScope_->currentIndex() == 0) invalidateMaterialPreview();
    }) : 0;
    refreshEntities();
}
void FindPanel::setGameContent(GameContent* content) {
    if (content_) disconnect(content_, nullptr, this, nullptr);
    content_ = content;
    if (content_) {
        connect(content_, &GameContent::contentChanged, this, [this] {
            invalidateMaterialPreview(); refreshProblems();
        });
        connect(content_, &GameContent::classesReady, this, [this] {
            invalidateMaterialPreview(); refreshProblems();
        });
    }
    refreshProblems();
}
editor::EntityReportFilter FindPanel::filter() const {
    editor::EntityReportFilter result;
    result.classname = class_->text().toStdString(); result.name = name_->text().toStdString();
    result.keyContains = keyContains_->text().toStdString();
    if (visgroup_->currentData().isValid()) result.visgroup = visgroup_->currentData().toInt();
    result.insideSelection = inside_->isChecked();
    if (brush_->currentIndex() == 1) result.brush = true;
    else if (brush_->currentIndex() == 2) result.brush = false;
    return result;
}
void FindPanel::refresh() { refreshEntities(); refreshProblems(); }
void FindPanel::refreshEntities() {
    if (!entities_) return;
    const QSignalBlocker blocker(entities_);
    entities_->setRowCount(0);
    if (!session_ || session_->document() != document_) return;
    std::set<document::NodeId> selected;
    for (const auto target : session_->selection().ordered())
        if (const auto* id = std::get_if<document::NodeId>(&target)) selected.insert(*id);
    const auto rows = editor::entityReport(*session_, filter());
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        entities_->insertRow(i);
        auto* cls = new QTableWidgetItem(text(rows[static_cast<std::size_t>(i)].classname));
        cls->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(rows[static_cast<std::size_t>(i)].entity));
        entities_->setItem(i, 0, cls);
        entities_->setItem(i, 1, new QTableWidgetItem(text(rows[static_cast<std::size_t>(i)].name)));
        entities_->setItem(i, 2, new QTableWidgetItem(QString::number(rows[static_cast<std::size_t>(i)].entity)));
        if (selected.contains(rows[static_cast<std::size_t>(i)].entity)) entities_->selectRow(i);
    }
    const QSignalBlocker visBlock(visgroup_);
    const auto keep = visgroup_->currentData();
    visgroup_->clear(); visgroup_->addItem(tr("Any visgroup"));
    if (document_) {
        const auto tree = editor::buildVisgroupTree(*document_);
        for (const auto& group : tree.nodes) visgroup_->addItem(text(group.name), group.vmfId);
    }
    if (keep.isValid()) visgroup_->setCurrentIndex(visgroup_->findData(keep));
}
void FindPanel::refreshProblems() {
    if (!problems_ || !document_) return;
    problems_->clear(); addons_->clear();
    // The report's names are as written in the map; each kind resolves differently (0.4 D11).
    // Sounds and particles come from the browsers' indexes, and an index still building counts as present
    // rather than flooding the report with false misses.
    const auto vfsPath = [](editor::AssetKind kind, std::string_view name) -> std::optional<std::string> {
        if (kind == editor::AssetKind::Material) return "materials/" + std::string(name) + ".vmt";
        if (kind == editor::AssetKind::Model) return std::string(name);
        return std::nullopt;
    };
    const auto exists = [this, vfsPath](editor::AssetKind kind, std::string_view name) {
        if (!content_) return false;
        if (const auto path = vfsPath(kind, name)) return content_->assetExists(*path);
        if (kind == editor::AssetKind::Sound) {
            const auto snapshot = content_->soundSnapshot();
            return !snapshot || snapshot->find(name) != nullptr;
        }
        if (kind == editor::AssetKind::Particle) {
            const auto snapshot = content_->particleSnapshot();
            return !snapshot || snapshot->find(name) != nullptr;
        }
        const auto* session = session_;
        return !session || editor::resolveInstanceFile(session->connectionMapPath(), name, session->connectionExtraRoot());
    };
    const auto provider = [this, vfsPath](editor::AssetKind kind, std::string_view name) -> std::optional<std::string> {
        const auto path = vfsPath(kind, name);
        return content_ && path ? content_->assetProviderName(*path) : std::nullopt;
    };
    // D15's unmounted-addon index arrives in 0.4.9; keep the callback seam here.
    const editor::AssetProvider unmountedAddon = [](editor::AssetKind, std::string_view) { return std::optional<std::string>{}; };
    const auto* classes = content_ ? content_->classes() : nullptr;
    editor::AssetReport report(*document_, exists, unmountedAddon, provider, classes);
    const auto known = [classes](std::string_view classname) { return !classes || classes->find(classname) != nullptr; };
    editor::Problems issues;
    if (session_ && session_->document() == document_)
        issues = report.problems(*document_, session_->connectionValidator(), known);
    else issues.missingAssets = report.missing();
    auto* missing = new QTreeWidgetItem(problems_, {tr("Missing assets (%1)").arg(issues.missingAssets.size()), {}});
    for (const auto& asset : issues.missingAssets) {
        auto* group = new QTreeWidgetItem(missing, {text(asset.path) + QStringLiteral(" · ") + assetKind(asset.kind), asset.addon ? tr("Would be provided by %1").arg(text(*asset.addon)) : QString()});
        for (const auto& ref : asset.references) {
            const auto label = ref.side ? tr("solid %1 side %2").arg(ref.solid).arg(*ref.side) : tr("entity %1 · %2").arg(ref.entity).arg(text(ref.key));
            auto* item = new QTreeWidgetItem(group, {label, {}});
            item->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(ref.entity));
            item->setData(0, Qt::UserRole + 1, QVariant::fromValue<qulonglong>(ref.solid));
        }
    }
    auto* invalid = new QTreeWidgetItem(problems_, {tr("Invalid connections (%1)").arg(issues.invalidConnections.size()), {}});
    for (const auto& problem : issues.invalidConnections) {
        auto* item = new QTreeWidgetItem(invalid, {tr("entity %1 · %2").arg(problem.entity).arg(text(problem.output)), tr("%1 issue(s)").arg(problem.issues.size())});
        item->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(problem.entity));
    }
    auto* unknown = new QTreeWidgetItem(problems_, {tr("Unknown classes (%1)").arg(issues.unknownClasses.size()), {}});
    for (const auto& item : issues.unknownClasses) {
        auto* row = new QTreeWidgetItem(unknown, {text(item.classname), tr("entity %1").arg(item.entity)});
        row->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(item.entity));
    }
    for (const auto& addon : report.addonsUsed()) {
        auto* item = new QTreeWidgetItem(addons_, {text(addon), {}});
        for (const auto& asset : report.missing()) if (asset.addon == addon) new QTreeWidgetItem(item, {text(asset.path), assetKind(asset.kind)});
    }
    problems_->expandAll(); addons_->expandAll();
}
void FindPanel::invalidateKeyPreview() { keyPreviewRows_.clear(); keyChanges_->setRowCount(0); keyApply_->setEnabled(false); }
void FindPanel::invalidateMaterialPreview() { materialPreviewRows_.clear(); materialChanges_->setRowCount(0); materialApply_->setEnabled(false); }
void FindPanel::selectEntities(const std::set<document::NodeId>& ids) {
    if (!session_ || !document_) return;
    session_->selection().setMode(editor::SelectionMode::Objects);
    std::vector<editor::Target> targets; for (auto id : ids) targets.emplace_back(id);
    session_->selection().replace(targets);
    emit frameRequested();
}
} // namespace apex::ui
