#pragma once

#include <QWidget>
#include "editor/asset_report.hpp"
#include "editor/entity_report.hpp"
#include "editor/find_replace.hpp"

class QCheckBox;
class QComboBox;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QTabWidget;
class QTreeWidget;

namespace apex::ui {
class GameContent;

class FindPanel final : public QWidget {
    Q_OBJECT
public:
    explicit FindPanel(QWidget* parent = nullptr);
    ~FindPanel() override;
    void setDocument(document::Document* document);
    void setEditorSession(editor::EditorSession* session);
    void setGameContent(GameContent* content);
    void refresh();
    QTableWidget* entityTable() const { return entities_; }
    QTreeWidget* problemsTree() const { return problems_; }
    QPushButton* keyPreviewButton() const { return keyPreview_; }
    QPushButton* keyApplyButton() const { return keyApply_; }
    QPushButton* materialPreviewButton() const { return materialPreview_; }
    QPushButton* materialApplyButton() const { return materialApply_; }
    QTableWidget* keyPreviewTable() const { return keyChanges_; }
    QTableWidget* materialPreviewTable() const { return materialChanges_; }

signals:
    void frameRequested();

private:
    void refreshEntities();
    void refreshProblems();
    void invalidateKeyPreview();
    void invalidateMaterialPreview();
    void selectEntities(const std::set<document::NodeId>& ids);
    editor::EntityReportFilter filter() const;
    document::Document* document_ = nullptr;
    editor::EditorSession* session_ = nullptr;
    GameContent* content_ = nullptr;
    std::size_t documentObserver_ = 0, selectionObserver_ = 0;
    QTabWidget* modes_ = nullptr;
    QLineEdit* class_ = nullptr;
    QLineEdit* name_ = nullptr;
    QLineEdit* keyContains_ = nullptr;
    QComboBox* visgroup_ = nullptr;
    QCheckBox* inside_ = nullptr;
    QComboBox* brush_ = nullptr;
    QTableWidget* entities_ = nullptr;
    QTreeWidget* problems_ = nullptr;
    QTreeWidget* addons_ = nullptr;
    QLineEdit *keyFind_ = nullptr, *keyReplace_ = nullptr;
    QCheckBox *wholeWord_ = nullptr, *caseSensitive_ = nullptr, *wildcard_ = nullptr, *connections_ = nullptr;
    QTableWidget* keyChanges_ = nullptr;
    QPushButton *keyPreview_ = nullptr, *keyApply_ = nullptr;
    QLineEdit *materialFind_ = nullptr, *materialReplace_ = nullptr;
    QCheckBox *wholeName_ = nullptr, *materialKeys_ = nullptr;
    QComboBox* materialScope_ = nullptr;
    QTableWidget* materialChanges_ = nullptr;
    QPushButton *materialPreview_ = nullptr, *materialApply_ = nullptr;
    std::vector<editor::KeyvalueChange> keyPreviewRows_;
    std::vector<editor::MaterialChange> materialPreviewRows_;
};

} // namespace apex::ui
