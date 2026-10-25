#pragma once

#include <QPointer>
#include <QWidget>
#include <memory>
#include <set>

#include "document/document.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListView;
class QListWidget;
class QSlider;
class QEvent;

namespace apex::editor { class EditorSession; }
namespace apex::ui { class GameContent; }

namespace apex::ui {

class MaterialsPanel final : public QWidget {
    Q_OBJECT
public:
    explicit MaterialsPanel(QWidget* parent = nullptr);
    ~MaterialsPanel() override;
    void setGameContent(GameContent* content);
    void setDocument(document::Document* document);
    void setEditorSession(editor::EditorSession* session);
    void setPickMode(bool enabled, const QString& initial = {}, const QString& what = {});
    QStringList filteredNames() const;
    std::size_t thumbnailRequestCount() const;

protected:
    void changeEvent(QEvent* event) override;
    bool eventFilter(QObject* object, QEvent* event) override;

signals:
    void activeMaterialRequested(const QString& name);
    void materialChosen(const QString& name);
    void pickCancelled();
    void statusMessage(const QString& message);

private:
    void refilter();
    void updateProgress();
    void updateGridSize();
    void updateSources();
    void updateShaders();
    void updateUsedMaterials();
    void activate(const QString& name);
    void applyToSelection(const QString& name);
    void remember(const QString& name);

    QPointer<GameContent> content_; // MainWindow destroys it before its child panels.
    std::string active_;
    document::Document* document_ = nullptr;
    editor::EditorSession* session_ = nullptr;
    std::size_t documentObserver_ = 0, sessionObserver_ = 0;
    std::size_t done_ = 0, total_ = 0;
    QLineEdit* nameFilter_ = nullptr;
    QComboBox* shaderFilter_ = nullptr;
    QComboBox* sourceFilter_ = nullptr; // data: kAnySource, kGameSource or an addon index
    QCheckBox* usedFilter_ = nullptr;
    QCheckBox* toolFilter_ = nullptr;
    QListWidget* recent_ = nullptr;
    QListView* grid_ = nullptr;
    QLabel* progress_ = nullptr;
    QLabel* pickBanner_ = nullptr;
    QSlider* size_ = nullptr;
    std::set<std::string> used_;
    bool pickMode_ = false;
    QString filterBeforePick_;
    QString pickMaterial_;
    class GridModel;
    GridModel* model_ = nullptr;
};

} // namespace apex::ui
