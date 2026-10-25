#pragma once

#include "editor/editor_session.hpp"

#include <QWidget>
#include <QSize>
#include <QMetaObject>
#include <functional>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QLabel;
class QVBoxLayout;
class QToolButton;
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
    std::vector<document::SideId> shown_; // The faces the fields show.
    bool queued_ = false, rebuilding_ = false;
    std::function<void(QString, QString, std::function<void(QString)>)> materialBrowser_;
};
} // namespace apex::ui
