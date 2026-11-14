#pragma once

#include "editor/displacement_edit.hpp"
#include "editor/editor_session.hpp"

#include <QWidget>
#include <QSize>
#include <QMetaObject>
#include <functional>
#include <optional>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QLabel;
class QVBoxLayout;
class QToolButton;
class QListWidget;
class QScrollArea;
class QResizeEvent;

namespace apex::ui {
class GameContent;
class UvView;
class FacePropertiesPanel final : public QWidget {
    Q_OBJECT
public:
    explicit FacePropertiesPanel(QWidget* parent = nullptr);
    ~FacePropertiesPanel() override;
    void setDocument(document::Document* document);
    void setEditorSession(editor::EditorSession* session);
    void setGameContent(GameContent* content);
    void setMaterialBrowser(std::function<void(QString, QString, std::function<void(QString)>)> browser);
    void refresh();
    QSize minimumSizeHint() const override { return {160, 96}; }

signals:
    void statusMessage(const QString& message);

protected:
    bool eventFilter(QObject* object, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void requestRefresh();
    bool fieldHasFocus() const;
    void edit(editor::FaceField field, const QString& value, const QString& token);
    document::Document* document_ = nullptr;
    editor::EditorSession* session_ = nullptr;
    std::size_t documentObserver_ = 0, sessionObserver_ = 0;
    QVBoxLayout* layout_ = nullptr;
    QWidget* content_ = nullptr;
    QScrollArea* controlsScroll_ = nullptr;
    UvView* uvView_ = nullptr;
    GameContent* gameContent_ = nullptr;
    QMetaObject::Connection thumbnailConnection_, contentConnection_;
    QLabel* hint_ = nullptr;
    QLineEdit* material_ = nullptr;
    QLineEdit* shiftU_ = nullptr;
    QLineEdit* shiftV_ = nullptr;
    QLineEdit* scaleU_ = nullptr;
    QLineEdit* scaleV_ = nullptr;
    QLineEdit* rotation_ = nullptr;
    QLineEdit* lightmap_ = nullptr;
    QCheckBox* treatAsOne_ = nullptr;
    std::vector<QToolButton*> smoothing_;
    std::vector<QToolButton*> alignment_;
    QToolButton* smoothingSummary_ = nullptr;
    QComboBox* extrapolation_ = nullptr;
    QLabel* displacement_ = nullptr; // Displacement section: summary (0.8.2) ...
    QWidget* displacementEdit_ = nullptr; // ... and its editable power and flags (0.8.3).
    QComboBox* displacementPower_ = nullptr;
    std::vector<std::pair<QCheckBox*, int>> displacementFlags_;
    int displacementFlagsAll_ = 0;
    void reportDisplacementEdit(const Result<editor::DisplacementEditReport>& result);
    QLabel* overlaysCaption_ = nullptr;
    QListWidget* overlays_ = nullptr; // Plan 0.8 D12: "Overlays on this face".
    void refreshOverlays(std::optional<document::SideId> side);
    std::vector<document::SideId> shown_; // The faces the fields show.
    bool queued_ = false, rebuilding_ = false;
    std::function<void(QString, QString, std::function<void(QString)>)> materialBrowser_;
};
} // namespace apex::ui
