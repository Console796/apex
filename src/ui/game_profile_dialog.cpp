#include "ui/game_profile_dialog.hpp"

#include "assets/path_expand.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

namespace apex::ui {
namespace {
QString fromPath(const std::filesystem::path& path) {
    return path.empty() ? QString() : QString::fromStdU16String(path.generic_u16string());
}
std::filesystem::path toPath(const QString& text) {
    return std::filesystem::path(text.trimmed().toStdU16String());
}
} // namespace

GameProfileDialog::GameProfileDialog(const assets::GameProfile* existing, QStringList takenNames, QWidget* parent)
    : QDialog(parent), takenNames_(std::move(takenNames)) {
    setWindowTitle(existing ? tr("Edit Game") : tr("Add Game"));
    setObjectName(QStringLiteral("gameProfileDialog"));
    resize(640, 0);
    auto* layout = new QVBoxLayout(this);
    auto* intro = new QLabel(tr("Choose the game's folder: the one holding gameinfo.txt (for example "
                                "<i>Half-Life 2/hl2</i> or <i>Portal 2/portal2</i>). The other fields are filled "
                                "from what is found there; check them before saving."), this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* form = new QFormLayout;
    directory_ = pathRow(tr("Game folder"), QStringLiteral("gameDirectory"), true, form);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("gameDirectoryStatus"));
    status_->setWordWrap(true);
    form->addRow(QString(), status_);
    name_ = new QLineEdit(this);
    name_->setObjectName(QStringLiteral("gameName"));
    form->addRow(tr("Name"), name_);
    connect(name_, &QLineEdit::textEdited, this, [this] { nameEdited_ = true; });

    fgds_ = new QListWidget(this);
    fgds_->setObjectName(QStringLiteral("gameFgds"));
    fgds_->setMaximumHeight(110);
    fgds_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    auto* fgdButtons = new QVBoxLayout;
    auto* addFgd = new QPushButton(tr("Add..."), this);
    auto* removeFgd = new QPushButton(tr("Remove"), this);
    fgdButtons->addWidget(addFgd);
    fgdButtons->addWidget(removeFgd);
    fgdButtons->addStretch();
    auto* fgdRow = new QHBoxLayout;
    fgdRow->addWidget(fgds_, 1);
    fgdRow->addLayout(fgdButtons);
    form->addRow(tr("Entity FGDs"), fgdRow);
    connect(addFgd, &QPushButton::clicked, this, [this] {
        const auto start = directory_->text().isEmpty() ? QString() : directory_->text();
        for (const auto& file : QFileDialog::getOpenFileNames(this, tr("Add FGD"), start, tr("FGD files (*.fgd)")))
            fgds_->addItem(file);
    });
    connect(removeFgd, &QPushButton::clicked, this, [this] { qDeleteAll(fgds_->selectedItems()); });

    escape_ = new QComboBox(this);
    escape_->setObjectName(QStringLiteral("gameEscapeMode"));
    escape_->addItem(tr("Strata Source (escape sequences in KeyValues)"));
    escape_->addItem(tr("Valve Source (no escape sequences)"));
    escape_->setToolTip(tr("How quotes and backslashes in KeyValues text are read and written. Use Valve Source for "
                           "games whose Hammer is Valve's, so saved maps open there unchanged."));
    form->addRow(tr("Engine branch"), escape_);

    rtLightModes_ = new QCheckBox(tr("Realtime light modes (specular, direct and indirect)"), this);
    rtLightModes_->setObjectName(QStringLiteral("gameRtLightModes"));
    rtLightModes_->setToolTip(tr("Strata Source's light_rt keys. Leave off for engines that only have plain VRAD lights, "
                                 "so the preview draws no light specular."));
    form->addRow(tr("Engine features"), rtLightModes_);
    pbrShader_ = new QCheckBox(tr("PBR material shader"), this);
    pbrShader_->setObjectName(QStringLiteral("gamePbrShader"));
    pbrShader_->setToolTip(tr("Strata Source's \"pbr\" shader. Leave off if the game does not have it."));
    form->addRow(QString(), pbrShader_);

    toolDirectory_ = pathRow(tr("Tool folder"), QStringLiteral("gameToolDirectory"), true, form);
    toolDirectory_->setPlaceholderText(tr("bin/linux64 or bin/win64 beside the game folder"));
    vbsp_ = pathRow(tr("vbsp"), QStringLiteral("gameVbsp"), false, form);
    vvis_ = pathRow(tr("vvis"), QStringLiteral("gameVvis"), false, form);
    vrad_ = pathRow(tr("vrad"), QStringLiteral("gameVrad"), false, form);
    game_ = pathRow(tr("Game executable"), QStringLiteral("gameExecutable"), false, form);
    for (auto* edit : {vbsp_, vvis_, vrad_, game_}) edit->setPlaceholderText(tr("Found in the tool folder"));
    layout->addLayout(form);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    connect(directory_, &QLineEdit::editingFinished, this, [this] {
        if (toPath(directory_->text()) != base_.gameDirectory) setGameDirectory(directory_->text());
    });

    if (existing) {
        editing_ = true;
        base_ = *existing;
        takenNames_.removeAll(QString::fromStdString(existing->name));
        load(base_);
    }
}

QLineEdit* GameProfileDialog::pathRow(const QString& label, const QString& objectName, bool directory, QFormLayout* form) {
    auto* edit = new QLineEdit(this);
    edit->setObjectName(objectName);
    auto* browse = new QPushButton(tr("Browse..."), this);
    auto* row = new QHBoxLayout;
    row->addWidget(edit, 1);
    row->addWidget(browse);
    form->addRow(label, row);
    connect(browse, &QPushButton::clicked, this, [this, edit, directory, label] {
        const auto start = edit->text().isEmpty() ? directory_->text() : edit->text();
        const auto picked = directory ? QFileDialog::getExistingDirectory(this, label, start)
                                      : QFileDialog::getOpenFileName(this, label, start);
        if (picked.isEmpty()) return;
        if (edit == directory_) setGameDirectory(picked);
        else edit->setText(picked);
    });
    return edit;
}

void GameProfileDialog::setGameDirectory(const QString& directory) {
    auto suggested = assets::suggestGameProfile(toPath(directory));
    if (!suggested) {
        base_.gameDirectory = toPath(directory);
        directory_->setText(directory);
        status_->setText(tr("<b>Not a game folder:</b> %1").arg(QString::fromStdString(suggested.error().what())));
        return;
    }
    // A name the user typed (or the edited profile's name) survives; otherwise gameinfo's name is used.
    const auto typed = name_->text().trimmed();
    const bool keepName = nameEdited_ || editing_;
    base_ = std::move(*suggested);
    load(base_);
    if (keepName && !typed.isEmpty()) name_->setText(typed);
}

void GameProfileDialog::load(const assets::GameProfile& profile) {
    directory_->setText(fromPath(profile.gameDirectory));
    const bool found = !profile.gameDirectory.empty() &&
                       !assets::detail::expand(profile.gameDirectory / "gameinfo.txt").empty();
    status_->setText(found ? tr("gameinfo.txt found.") : tr("<b>No gameinfo.txt in this folder.</b>"));
    name_->setText(QString::fromStdString(profile.name));
    fgds_->clear();
    for (const auto& fgd : profile.fgds) fgds_->addItem(fromPath(fgd));
    escape_->setCurrentIndex(profile.escapeMode == kv1::EscapeMode::Strata ? 0 : 1);
    rtLightModes_->setChecked(profile.features.rtLightModes);
    pbrShader_->setChecked(profile.features.pbrShader);
    toolDirectory_->setText(fromPath(profile.customToolDirectory));
    auto tool = [&](const char* name) {
        auto it = profile.toolPaths.find(name);
        return it == profile.toolPaths.end() ? QString() : fromPath(it->second);
    };
    vbsp_->setText(tool("vbsp"));
    vvis_->setText(tool("vvis"));
    vrad_->setText(tool("vrad"));
    game_->setText(tool("game"));
}

assets::GameProfile GameProfileDialog::profile() const {
    auto result = base_;
    result.name = name_->text().trimmed().toStdString();
    result.gameDirectory = toPath(directory_->text());
    result.fgds.clear();
    for (int i = 0; i < fgds_->count(); ++i) result.fgds.push_back(toPath(fgds_->item(i)->text()));
    result.escapeMode = escape_->currentIndex() == 0 ? kv1::EscapeMode::Strata : kv1::EscapeMode::Legacy;
    result.features = {rtLightModes_->isChecked(), pbrShader_->isChecked()};
    result.customToolDirectory = toPath(toolDirectory_->text());
    for (auto [key, edit] : {std::pair{"vbsp", vbsp_}, std::pair{"vvis", vvis_}, std::pair{"vrad", vrad_},
                             std::pair{"game", game_}}) {
        if (edit->text().trimmed().isEmpty()) result.toolPaths.erase(key);
        else result.toolPaths[key] = toPath(edit->text());
    }
    return result;
}

QString GameProfileDialog::validate() const {
    const auto current = profile();
    if (current.gameDirectory.empty()) return tr("Choose the game folder.");
    if (assets::detail::expand(current.gameDirectory / "gameinfo.txt").empty())
        return tr("The game folder has no gameinfo.txt.");
    if (current.name.empty()) return tr("Give the game a name.");
    if (takenNames_.contains(QString::fromStdString(current.name)))
        return tr("A game named \"%1\" already exists.").arg(QString::fromStdString(current.name));
    return {};
}

void GameProfileDialog::accept() {
    if (const auto problem = validate(); !problem.isEmpty()) {
        QMessageBox::warning(this, windowTitle(), problem);
        return;
    }
    QDialog::accept();
}

} // namespace apex::ui
