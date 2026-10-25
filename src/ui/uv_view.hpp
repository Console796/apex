#pragma once

#include "editor/face_edit.hpp"

#include <QPointF>
#include <QWidget>

class QPainter;

namespace apex::document { class Document; }
namespace apex::editor { class EditorSession; }
namespace apex::ui { class GameContent; }

namespace apex::ui {

class UvView final : public QWidget {
    Q_OBJECT
public:
    explicit UvView(QWidget* parent = nullptr);
    ~UvView() override;
    QSize minimumSizeHint() const override { return {120, 120}; }
    void setDocument(document::Document* document);
    void setEditorSession(editor::EditorSession* session);
    void setGameContent(GameContent* content);

signals:
    void statusMessage(const QString& message);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    enum class Drag { None, Pan, Shift, Pivot, Rotate, ScaleU, ScaleV };
    void syncFace();
    void frameFace();
    QPointF toScreen(DVec3 point) const;
    DVec3 fromScreen(QPointF point) const;
    QTransform textureTransform(const editor::FaceSnapshot& face, int textureWidth, int textureHeight,
                                const geometry::FacePlaneFrame& frame) const;
    Drag hitTest(QPointF point, double* edgeTexel) const;
    void applyGesture(editor::FaceGesture gesture, double a, double b);
    void finishDrag(bool cancel);
    document::Document* document_ = nullptr;
    editor::EditorSession* session_ = nullptr;
    GameContent* content_ = nullptr;
    std::size_t documentObserver_ = 0, sessionObserver_ = 0;
    std::optional<editor::FaceSnapshot> active_;
    std::vector<editor::FaceSnapshot> selected_;
    geometry::FacePlaneFrame frame_;
    document::SideId activeSide_ = -1;
    std::vector<DVec3> activePoints_;
    std::uint64_t gestureState_ = 0;
    DVec3 pivot_{};
    QPointF pan_{}, pressPoint_{}, lastPoint_{};
    double zoom_ = 1;
    Drag drag_ = Drag::None;
    Drag hover_ = Drag::None;
    bool snap_ = true, changed_ = false, stale_ = true;
    double startAngle_ = 0, lastAngle_ = 0, lastScale_ = 1;
    double lastAppliedU_ = 0, lastAppliedV_ = 0;
    double edgeTexel_ = 0;
    geometry::FaceUv dragUv_;
    QString token_;
};

} // namespace apex::ui
