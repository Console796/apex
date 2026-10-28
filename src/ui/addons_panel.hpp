#pragma once

#include <QWidget>
#include <QPointer>

class QLabel;
class QLineEdit;
class QTreeWidget;

namespace apex::ui {
class GameContent;

class AddonsPanel final : public QWidget {
    Q_OBJECT
public:
    explicit AddonsPanel(QWidget* parent = nullptr);
    void setGameContent(GameContent* content);
private:
    void refresh();
    void filter();
    QPointer<GameContent> content_;
    QLineEdit* filter_ = nullptr;
    QTreeWidget* entries_ = nullptr;
    QLabel* status_ = nullptr;
};
} // namespace apex::ui
