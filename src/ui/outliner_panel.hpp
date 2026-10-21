#pragma once

#include <QTreeWidget>

namespace apex::document {
class Document;
}

namespace apex::ui {

// Read-only outliner: World, Entities, Groups and Visgroups of the open document (plan 0.1,
// 0.1.10). Rebuilt on setDocument() and refresh(); nothing is editable.
class OutlinerPanel : public QTreeWidget {
    Q_OBJECT
public:
    explicit OutlinerPanel(QWidget* parent = nullptr);

    void setDocument(const document::Document* document);
    void refresh();

private:
    const document::Document* document_ = nullptr;
};

}  // namespace apex::ui
