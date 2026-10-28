#pragma once

#include "document/cordon.hpp"
#include "editor/editor_session.hpp"

#include <QWidget>
#include <array>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QPushButton;

namespace apex::ui {

// Lists the map's cordons and edits the selected one's box (0.5 D7). Every edit is one undo step in the document.
class CordonEditor final : public QWidget {
    Q_OBJECT
public:
    explicit CordonEditor(QWidget* parent = nullptr);
    ~CordonEditor() override;
    void setDocument(document::Document* document);
    void setEditorSession(editor::EditorSession* session) { session_ = session; }
    // The selection's bounds become a new cordon (or the selected one's box when `replace`).
    void fromSelection(bool replace);

private:
    void reload();
    void select(int row);
    void boxEdited();
    std::optional<document::NodeId> current() const;
    document::Document* document_ = nullptr;
    editor::EditorSession* session_ = nullptr;
    std::size_t token_ = 0;
    bool observing_ = false;
    bool updating_ = false;
    QComboBox* list_;
    QCheckBox* active_;
    std::array<QDoubleSpinBox*, 6> fields_{};
    QPushButton* add_;
    QPushButton* fromSelection_;
    QPushButton* remove_;
};

} // namespace apex::ui
