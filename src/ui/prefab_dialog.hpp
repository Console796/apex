#pragma once

#include <QDialog>
#include <QStringList>
#include <filesystem>

#include "editor/editor_session.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;

namespace apex::ui {

// Create Prefab from Selection (plan 0.4.7): a name and an optional category subfolder (existing ones are
// offered; '/' nests). Accepting writes the selection to `<folder>/<category>/<name>.vmf` through
// editor::createPrefab; an existing file is replaced only with the checkbox.
class PrefabDialog final : public QDialog {
    Q_OBJECT
public:
    PrefabDialog(editor::EditorSession& session, std::filesystem::path folder, QStringList categories,
                 QWidget* parent = nullptr);

    QString prefabName() const;
    QString category() const;
    void setPrefabName(const QString& name);
    void setCategory(const QString& category);
    void setOverwrite(bool overwrite);
    bool create(); // The accept step without closing: false leaves the reason in error().
    QString error() const;
    const std::filesystem::path& createdFile() const { return created_; }

signals:
    void created(QString file);

public slots:
    void accept() override;

private:
    editor::EditorSession& session_;
    std::filesystem::path folder_, created_;
    QLineEdit* name_ = nullptr;
    QComboBox* category_ = nullptr;
    QCheckBox* overwrite_ = nullptr;
    QLabel* error_ = nullptr;
};

}  // namespace apex::ui
