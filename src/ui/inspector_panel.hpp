#pragma once

#include "editor/editor_session.hpp"
#include <QWidget>
#include <functional>

class QCheckBox;
class QComboBox;
class QVBoxLayout;

namespace apex::ui {

class InspectorPanel : public QWidget {
    Q_OBJECT
public:
    explicit InspectorPanel(QWidget* parent = nullptr);
    ~InspectorPanel() override;
    void setDocument(document::Document* document);
    void setEditorSession(editor::EditorSession* session);
    void setMaterialBrowser(std::function<void(QString, std::function<void(QString)>)> browser);
    void refresh();

protected:
    bool eventFilter(QObject* object, QEvent* event) override;

private:
    void requestRefresh();
    bool current(const std::vector<document::NodeId>& entities) const;
    void watch(QWidget* field);
    document::Document* document_ = nullptr;
    editor::EditorSession* session_ = nullptr;
    std::size_t documentObserver_ = 0, sessionObserver_ = 0;
    QComboBox* classPicker_ = nullptr;
    QCheckBox* raw_ = nullptr;
    QWidget* rows_ = nullptr;
    QVBoxLayout* layout_ = nullptr;
    std::vector<document::NodeId> entities_;
    std::uint64_t selectionRevision_ = 0, classesRevision_ = 0;
    bool queued_ = false, rebuilding_ = false;
    std::function<void(QString, std::function<void(QString)>)> materialBrowser_;
};

} // namespace apex::ui
