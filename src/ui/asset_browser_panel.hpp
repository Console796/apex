#pragma once

#include <QPointer>
#include <QWidget>
#include <set>
#include <functional>

#include "assets/asset_index.hpp"
#include "document/document.hpp"

class QCheckBox;
class QAbstractItemView;
class QComboBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QVBoxLayout;

namespace apex::ui {
class GameContent;

// Common browser controls and document observation. Hidden browsers defer filtering and document
// scans until shown (or explicitly queried by a caller); they never start asset work themselves.
class AssetBrowserPanel : public QWidget {
    Q_OBJECT
public:
    enum class Kind { Model, Sound, Particle };
    ~AssetBrowserPanel() override;
    void setGameContent(GameContent* content);
    void setDocument(document::Document* document);
    QStringList filteredNames();
    void setPickMode(bool enabled, const QString& initial = {}, const QString& what = {});

signals:
    void assetChosen(QString name);
    void pickCancelled();

protected:
    AssetBrowserPanel(Kind kind, QString prefix, QWidget* parent);
    void showEvent(QShowEvent* event) override;
    bool eventFilter(QObject* object, QEvent* event) override;
    void setPickView(QAbstractItemView* view, std::function<void()> activate = {});
    void selectName(const QString& name);
    bool pickMode() const { return pickMode_; }
    virtual QString pickedValue(const QString& name) const { return name; }
    void refresh();
    void invalidate();
    void remember(const QString& name);
    assets::AssetFilter filter() const;
    QString sourceName(std::optional<std::size_t> mount) const;
    virtual void rebuild() = 0;
    virtual void contentReset() {}
    virtual void recentActivated(const QString&) {}
    virtual void pickModeChanged(bool) {}
    void setCount(std::size_t count);
    template <typename Snapshot> void updateSources(const Snapshot& snapshot) {
        std::set<std::optional<std::size_t>> sources;
        for (const auto& entry : snapshot.entries) sources.insert(entry.mount);
        updateSources(sources);
    }
    QPointer<GameContent> content_;
    QVBoxLayout* layout_ = nullptr;
    QHBoxLayout* filters_ = nullptr;
    QLineEdit* nameFilter_ = nullptr;
    QComboBox* sourceFilter_ = nullptr;
    QCheckBox* usedFilter_ = nullptr;
    QLabel* progress_ = nullptr;
    QListWidget* recent_ = nullptr;
    QStringList names_;

private:
    void updateSources(const std::set<std::optional<std::size_t>>& sources);
    void collectUsed();
    void updateProgress(std::size_t done, std::size_t total);
    void chooseCurrent();
    QAbstractItemView* pickView_ = nullptr;
    QLabel* pickBanner_ = nullptr;
    QPushButton* choose_ = nullptr;
    bool pickMode_ = false;
    QString filterBeforePick_, initial_;
    Kind kind_;
    QString prefix_;
    document::Document* document_ = nullptr;
    std::size_t observer_ = 0;
    std::set<std::string> used_;
    bool dirty_ = true, usedDirty_ = true;
};
} // namespace apex::ui
