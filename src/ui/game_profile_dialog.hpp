#pragma once

#include "assets/game_profile.hpp"

#include <QDialog>
#include <QStringList>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QListWidget;

namespace apex::ui {

// Add Game / Edit Game: a game profile for any Source game with a gameinfo.txt. Picking the game
// folder fills every other field from suggestGameProfile(); the user corrects what it guessed.
class GameProfileDialog final : public QDialog {
    Q_OBJECT
public:
    // `takenNames` are profile names already in use; a profile must not reuse one (the picker selects by name).
    GameProfileDialog(const assets::GameProfile* existing, QStringList takenNames, QWidget* parent = nullptr);
    // Sets the game folder and refills the other fields from what is found there.
    void setGameDirectory(const QString& directory);
    assets::GameProfile profile() const;
    // Empty when the fields describe a loadable profile, else the reason shown to the user.
    QString validate() const;
    void accept() override;

private:
    void load(const assets::GameProfile& profile);
    QLineEdit* pathRow(const QString& label, const QString& objectName, bool directory, QFormLayout* form);
    assets::GameProfile base_;
    QStringList takenNames_;
    bool nameEdited_ = false;
    bool editing_ = false;
    QLineEdit* directory_;
    QLabel* status_;
    QLineEdit* name_;
    QListWidget* fgds_;
    QComboBox* escape_;
    QCheckBox* rtLightModes_;
    QCheckBox* pbrShader_;
    QDoubleSpinBox* textureScale_;
    QComboBox* textureFilter_;
    QLineEdit* toolDirectory_;
    QLineEdit* vbsp_;
    QLineEdit* vvis_;
    QLineEdit* vrad_;
    QLineEdit* game_;
};

} // namespace apex::ui
