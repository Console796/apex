#pragma once

#include "editor/actions.hpp"

#include <QDialog>
#include <optional>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTreeWidget;
class QTreeWidgetItem;

namespace apex::ui {

class MainWindow;

// Modal "press a key" prompt. Records one chord; for actions that allow it, a bare Ctrl/Alt/Shift
// pressed and released alone records a held-key binding.
class KeyCaptureDialog : public QDialog {
    Q_OBJECT
public:
    KeyCaptureDialog(const QString& actionName, bool allowHeldModifier, QWidget* parent = nullptr);
    const std::optional<editor::KeyChord>& chord() const { return chord_; }

protected:
    bool event(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;

private:
    bool allowHeld_;
    bool sawKey_ = false;
    std::optional<editor::KeyChord> chord_;
    QLabel* hint_ = nullptr;
    QString pendingModifier_;
};

// Preferences: general settings and the keybinding editor with savable presets.
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(MainWindow& window);

    // Edits the keybinding working copy (exposed for tests).
    const editor::Preset& workingPreset() const { return working_; }
    bool keysModified() const;

private:
    QWidget* buildGeneralPage();
    QWidget* buildKeysPage();
    void loadPreset(const std::string& name);
    void refreshPresetCombo();
    void refreshTree();
    void refreshButtons();
    editor::ActionId selectedAction() const;
    void captureKey(bool additional);
    void assign(const editor::ActionId& id, std::vector<editor::KeyChord> chords);
    void resetSelected();
    bool saveAs();
    bool saveCurrent();
    void deletePreset();
    void importPreset();
    void exportPreset();
    bool applyAll();
    bool confirmDiscard();

    MainWindow& window_;
    editor::Preset working_;
    editor::Preset original_;
    bool baseIsBuiltin_ = true;

    QLineEdit* newSolidMaterial_ = nullptr;
    QComboBox* uvAlignment_ = nullptr;
    QComboBox* theme_ = nullptr;
    QCheckBox* autosave_ = nullptr;
    QSpinBox* autosaveMinutes_ = nullptr;
    QSpinBox* undoMiB_ = nullptr;
    QComboBox* inspectorOpens_ = nullptr;

    QComboBox* presetCombo_ = nullptr;
    QLabel* modified_ = nullptr;
    QLineEdit* filter_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QLabel* detail_ = nullptr;
    QPushButton* assignButton_ = nullptr;
    QPushButton* addButton_ = nullptr;
    QPushButton* clearButton_ = nullptr;
    QPushButton* resetButton_ = nullptr;
    QPushButton* saveButton_ = nullptr;
    QPushButton* saveAsButton_ = nullptr;
    QPushButton* deleteButton_ = nullptr;
};

}  // namespace apex::ui
