#pragma once

#include "editor/editor_session.hpp"
#include <QTimer>
#include <QWidget>

namespace apex::ui {

editor::PieSettings loadPieSettings();
void savePieSettings(const editor::PieSettings& settings);

class PieMenuPopup final : public QWidget {
    Q_OBJECT
public:
    explicit PieMenuPopup(editor::EditorSession& session, QWidget* parent);
    void open(QWidget* pane);
    void sync();
    QPointF centre() const { return centre_; }
    QRectF drawingBounds() const;

protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent*) override;
    void hideEvent(QHideEvent*) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;

private:
    editor::EditorSession& session_;
    QTimer timer_;
    QPointF centre_, origin_, displacement_, cursor_;
    QSizeF labelSize_;
    bool closing_ = false;
    void mouse(QMouseEvent* event);
    void key(QKeyEvent* event);
    void send(editor::InputEvent event);
    QString label(std::size_t index) const;
};
} // namespace apex::ui
