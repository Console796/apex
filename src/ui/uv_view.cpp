#include "ui/uv_view.hpp"

#include <QImage>
#include <QKeyEvent>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QTransform>
#include <QWheelEvent>

#include <algorithm>
#include <atomic>
#include <cmath>

#include "editor/editor_session.hpp"
#include "ui/game_content.hpp"
#include "ui/theme.hpp"

namespace apex::ui {
namespace {
std::atomic<std::uint64_t> nextDrag{1};
QString fromStd(std::string_view s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }
}

UvView::UvView(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("faceUvView"));
    setMinimumSize(120, 120);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
}
UvView::~UvView() {
    setEditorSession(nullptr);
    setDocument(nullptr);
}
void UvView::setDocument(document::Document* document) {
    if (document_) document_->unobserve(documentObserver_);
    document_ = document;
    if (document_) documentObserver_ = document_->observe([this](const document::ChangeSet&) { stale_ = true; update(); });
    activeSide_ = -1;
    stale_ = true;
    update();
}
void UvView::setEditorSession(editor::EditorSession* session) {
    if (session_) session_->unobserve(sessionObserver_);
    session_ = session;
    if (session_) sessionObserver_ = session_->observe([this] { stale_ = true; update(); });
    activeSide_ = -1;
    stale_ = true;
    update();
}
void UvView::setGameContent(GameContent* content) {
    if (content_) disconnect(content_, nullptr, this, nullptr);
    content_ = content;
    if (content_) {
        connect(content_, &GameContent::thumbnailsReady, this, qOverload<>(&UvView::update));
        connect(content_, &GameContent::contentChanged, this, qOverload<>(&UvView::update));
    }
    update();
}
void UvView::syncFace() {
    // Snapshots are rebuilt only when the document or session changed, not per hover or paint.
    if (!stale_) return;
    stale_ = false;
    active_.reset();
    selected_.clear();
    if (!session_ || !document_ || session_->document() != document_ ||
        session_->selection().mode() != editor::SelectionMode::Faces) return;
    for (const auto target : session_->selection().ordered()) {
        const auto side = std::get<document::SideId>(target);
        if (auto face = editor::faceSnapshot(*document_, side)) {
            if (session_->selection().active() == editor::Target{side}) active_ = face;
            selected_.push_back(std::move(*face));
        }
    }
    if (!active_ && !selected_.empty()) active_ = selected_.back();
    if (!active_) { activeSide_ = -1; return; }
    const auto next = static_cast<document::SideId>(active_->face.sideId);
    if (next != activeSide_) {
        activeSide_ = next;
        activePoints_ = active_->points;
        frame_ = geometry::planeFrame(active_->face.plane.normal, active_->points);
        pivot_ = frame_.origin;
        pan_ = {};
        frameFace();
    } else if (active_->points != activePoints_) {
        // Same face, new geometry (a move or vertex edit): refit the frame, keep the view.
        activePoints_ = active_->points;
        frame_ = geometry::planeFrame(active_->face.plane.normal, active_->points);
        pivot_ = frame_.origin;
    }
}
UvView::Drag UvView::hitTest(QPointF point, double* edgeTexel) const {
    if (!active_ || !session_) return Drag::None;
    if (QLineF(point, toScreen(pivot_)).length() <= 9) return Drag::Pivot;
    if (QLineF(point, toScreen(pivot_) + QPointF(0, -42)).length() <= 9) return Drag::Rotate;
    const auto [w, h] = session_->faceTextureSize(active_->face.material);
    const auto mapping = textureTransform(*active_, w, h, frame_);
    if (!mapping.isInvertible()) return Drag::None;
    const auto inverse = mapping.inverted();
    const auto image = inverse.map(point), pivot = inverse.map(toScreen(pivot_));
    // Screen distance to a tile edge is the image-space gap over that coordinate's screen gradient.
    const auto gradU = std::hypot(inverse.m11(), inverse.m21()), gradV = std::hypot(inverse.m12(), inverse.m22());
    const auto lineU = std::round(image.x() / 512.0) * 512.0, lineV = std::round(image.y() / 512.0) * 512.0;
    const auto edgeU = std::abs(image.x() - lineU) / gradU, edgeV = std::abs(image.y() - lineV) / gradV;
    // An edge through the pivot has nothing to scale against.
    const bool scalesU = edgeU <= 8 && std::abs(lineU - pivot.x()) / gradU > 8;
    const bool scalesV = edgeV <= 8 && std::abs(lineV - pivot.y()) / gradV > 8;
    if (scalesU && (!scalesV || edgeU <= edgeV)) {
        if (edgeTexel) *edgeTexel = lineU / 512.0 * w;
        return Drag::ScaleU;
    }
    if (scalesV) {
        if (edgeTexel) *edgeTexel = lineV / 512.0 * h;
        return Drag::ScaleV;
    }
    return Drag::Shift;
}
void UvView::applyGesture(editor::FaceGesture gesture, double a, double b) {
    // History moved under the drag (an undo mid-drag): end it rather than write onto another state.
    if (changed_ && document_->historyState() != gestureState_) { finishDrag(false); return; }
    const auto result = session_->editFaceGesture(gesture, a, b, pivot_, token_.toStdString());
    if (!result) { emit statusMessage(fromStd(result.error().what())); return; }
    if (*result != 0) { changed_ = true; gestureState_ = document_->historyState(); }
}
void UvView::frameFace() {
    if (!active_ || width() <= 0 || height() <= 0) return;
    double minX = INFINITY, minY = INFINITY, maxX = -INFINITY, maxY = -INFINITY;
    for (const auto p : active_->points) {
        const auto d = p - frame_.origin;
        const auto x = glm::dot(d, frame_.x), y = glm::dot(d, frame_.y);
        minX = std::min(minX, x); maxX = std::max(maxX, x);
        minY = std::min(minY, y); maxY = std::max(maxY, y);
    }
    const auto spanX = std::max(1e-6, maxX - minX), spanY = std::max(1e-6, maxY - minY);
    zoom_ = std::clamp(std::min((width() - 32.0) / spanX, (height() - 32.0) / spanY), 0.05, 100.0);
    pan_ = {};
    update();
}
QPointF UvView::toScreen(DVec3 point) const {
    const auto delta = point - frame_.origin;
    return QPointF(width() / 2.0, height() / 2.0) + pan_ + zoom_ * QPointF(glm::dot(delta, frame_.x), glm::dot(delta, frame_.y));
}
DVec3 UvView::fromScreen(QPointF point) const {
    const auto local = (point - QPointF(width() / 2.0, height() / 2.0) - pan_) / zoom_;
    return frame_.origin + frame_.x * local.x() + frame_.y * local.y();
}
QTransform UvView::textureTransform(const editor::FaceSnapshot& face, int textureWidth, int textureHeight,
                                    const geometry::FacePlaneFrame& frame) const {
    const auto& uv = face.face.uv;
    const auto a = glm::dot(frame.x, uv.uaxis) / uv.scaleU;
    const auto b = glm::dot(frame.x, uv.vaxis) / uv.scaleV;
    const auto c = glm::dot(frame.y, uv.uaxis) / uv.scaleU;
    const auto d = glm::dot(frame.y, uv.vaxis) / uv.scaleV;
    const auto det = a * d - b * c;
    if (!std::isfinite(det) || std::abs(det) < 1e-12) return QTransform(0, 0, 0, 0, 0, 0);
    const auto u0 = glm::dot(frame.origin, uv.uaxis) / uv.scaleU + uv.shiftU;
    const auto v0 = glm::dot(frame.origin, uv.vaxis) / uv.scaleV + uv.shiftV;
    const auto x0 = (-d * u0 + c * v0) / det;
    const auto y0 = (b * u0 - a * v0) / det;
    const auto center = QPointF(width() / 2.0, height() / 2.0) + pan_ + zoom_ * QPointF(x0, y0);
    constexpr double sampleSize = 512; // Image units per tile.
    const double su = textureWidth / sampleSize, sv = textureHeight / sampleSize;
    return {zoom_ * d * su / det, -zoom_ * b * su / det, -zoom_ * c * sv / det, zoom_ * a * sv / det,
            center.x(), center.y()};
}
void UvView::paintEvent(QPaintEvent*) {
    syncFace();
    const auto& t = currentTokens();
    QPainter p(this);
    p.fillRect(rect(), t.base);
    p.setRenderHint(QPainter::Antialiasing);
    if (!active_) {
        p.setPen(t.textDim);
        p.drawText(rect(), Qt::AlignCenter, tr("Select faces to edit UVs"));
        return;
    }
    const auto [w, h] = session_->faceTextureSize(active_->face.material);
    QPixmap texture(512, 512);
    texture.fill(t.raised);
    if (content_ && content_->thumbnails()) {
        // The transform maps one tile to 512 image units whatever the thumbnail's own size.
        if (const auto thumb = content_->thumbnails()->request(active_->face.material, 512))
            texture = QPixmap::fromImage(QImage(reinterpret_cast<const uchar*>(thumb->rgba.data()),
                thumb->width, thumb->height, thumb->width * 4, QImage::Format_RGBA8888)
                .scaled(512, 512, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
    }
    const auto mapping = textureTransform(*active_, w, h, frame_);
    if (mapping.isInvertible()) {
        p.save();
        p.setWorldTransform(mapping);
        // Tiles cover the visible area wherever the face sits, capped for extreme zoom-outs.
        const auto visible = mapping.inverted().mapRect(QRectF(rect()));
        const auto left = std::floor(visible.left() / 512) * 512, top = std::floor(visible.top() / 512) * 512;
        const auto right = std::ceil(visible.right() / 512) * 512, bottom = std::ceil(visible.bottom() / 512) * 512;
        if ((right - left) / 512 * ((bottom - top) / 512) <= 4096) {
            p.setOpacity(0.78);
            p.drawTiledPixmap(QRectF(left, top, right - left, bottom - top), texture);
            p.setOpacity(1);
            p.setPen(QPen(t.border, 1));
            for (auto at = left; at <= right; at += 512) p.drawLine(QPointF(at, top), QPointF(at, bottom));
            for (auto at = top; at <= bottom; at += 512) p.drawLine(QPointF(left, at), QPointF(right, at));
        }
        QPen major(t.accent, 2);
        p.setPen(major);
        p.drawRect(QRectF(0, 0, 512, 512));
        p.restore();
    }
    p.save();
    p.translate(width() / 2.0 + pan_.x(), height() / 2.0 + pan_.y());
    p.scale(zoom_, zoom_);
    const auto drawFace = [&](const editor::FaceSnapshot& face, QColor color, qreal alpha) {
        if (face.face.sideId == active_->face.sideId ||
            std::abs(std::abs(glm::dot(face.face.plane.normal, active_->face.plane.normal)) - 1.0) > 1e-5 ||
            std::abs(glm::dot(active_->face.plane.normal, face.points.front() - active_->points.front())) > 1e-4) return;
        QPolygonF polygon;
        for (const auto point : face.points) {
            const auto d = point - frame_.origin;
            polygon << QPointF(glm::dot(d, frame_.x), glm::dot(d, frame_.y));
        }
        color.setAlphaF(static_cast<float>(alpha));
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(color, 2 / zoom_));
        p.drawPolygon(polygon);
    };
    const auto scenePoint = [this](DVec3 point) {
        const auto d = point - frame_.origin;
        return QPointF(glm::dot(d, frame_.x), glm::dot(d, frame_.y));
    };
    for (const auto& face : selected_) drawFace(face, t.textDim, 0.6);
    QPolygonF activePolygon;
    for (const auto point : active_->points) activePolygon << scenePoint(point);
    p.setBrush(QColor(t.selection.red(), t.selection.green(), t.selection.blue(), 48));
    p.setPen(QPen(t.vertexSelected, 2 / zoom_));
    p.drawPolygon(activePolygon);
    for (const auto point : active_->points) p.drawEllipse(scenePoint(point), 3.5 / zoom_, 3.5 / zoom_);
    const auto pivotScene = scenePoint(pivot_);
    p.setBrush(hover_ == Drag::Pivot ? t.accentHover : t.accent);
    p.setPen(QPen(t.text, 1));
    p.drawEllipse(pivotScene, 5 / zoom_, 5 / zoom_);
    const QPointF rotateScreen = pivotScene + QPointF(0, -42 / zoom_);
    p.setBrush(hover_ == Drag::Rotate ? t.accentHover : t.gizmoHighlight);
    p.drawEllipse(rotateScreen, 5 / zoom_, 5 / zoom_);
    p.setPen(QPen(t.textDim, 1));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(pivotScene, 40 / zoom_, 40 / zoom_);
    p.restore();
}
void UvView::mousePressEvent(QMouseEvent* e) {
    syncFace();
    pressPoint_ = lastPoint_ = e->position();
    if (e->button() == Qt::MiddleButton) {
        if (e->type() == QEvent::MouseButtonDblClick) frameFace();
        else drag_ = Drag::Pan;
        return;
    }
    if (e->button() != Qt::LeftButton || !active_) return;
    const auto hit = hitTest(e->position(), &edgeTexel_);
    if (hit == Drag::None) return;
    drag_ = hit;
    if (drag_ == Drag::Rotate) {
        const auto d = e->position() - toScreen(pivot_);
        startAngle_ = std::atan2(d.y(), d.x());
        lastAngle_ = 0; // Degrees applied so far in this drag.
    }
    snap_ = !e->modifiers().testFlag(Qt::AltModifier);
    changed_ = false;
    lastScale_ = 1;
    lastAppliedU_ = lastAppliedV_ = 0;
    dragUv_ = active_->face.uv;
    token_ = QString::number(nextDrag.fetch_add(1));
    setFocus(Qt::MouseFocusReason);
}
void UvView::mouseMoveEvent(QMouseEvent* e) {
    const auto point = e->position();
    if (drag_ == Drag::None) {
        syncFace();
        const auto next = hitTest(point, nullptr);
        hover_ = next;
        setCursor(next == Drag::Pivot ? Qt::SizeAllCursor : next == Drag::Rotate ? Qt::CrossCursor :
                  next == Drag::ScaleU || next == Drag::ScaleV ? Qt::SizeAllCursor :
                  next == Drag::Shift ? Qt::OpenHandCursor : Qt::ArrowCursor);
        update();
        return;
    }
    if (drag_ == Drag::Pan) { pan_ += point - lastPoint_; lastPoint_ = point; update(); return; }
    if (!active_ || !session_) return;
    if (drag_ == Drag::Pivot) {
        pivot_ = fromScreen(point);
        double best = 8;
        for (const auto vertex : active_->points) {
            const auto d = QLineF(point, toScreen(vertex)).length();
            if (d < best) { best = d; pivot_ = vertex; }
        }
        if (QLineF(point, toScreen(frame_.origin)).length() < best) pivot_ = frame_.origin;
        update();
        lastPoint_ = point;
        return;
    }
    if (drag_ == Drag::Shift) {
        const auto movement = point - pressPoint_;
        const auto deltaWorld = movement / zoom_;
        auto du = -glm::dot(frame_.x * deltaWorld.x() + frame_.y * deltaWorld.y(), dragUv_.uaxis) / dragUv_.scaleU;
        auto dv = -glm::dot(frame_.x * deltaWorld.x() + frame_.y * deltaWorld.y(), dragUv_.vaxis) / dragUv_.scaleV;
        if (snap_ && active_) {
            const auto [w, h] = session_->faceTextureSize(active_->face.material);
            double bestU = 8, bestV = 8;
            const auto projectedU = dragUv_.uaxis - active_->face.plane.normal * glm::dot(dragUv_.uaxis, active_->face.plane.normal);
            const auto projectedV = dragUv_.vaxis - active_->face.plane.normal * glm::dot(dragUv_.vaxis, active_->face.plane.normal);
            const auto pxU = zoom_ * std::abs(dragUv_.scaleU) / std::max(1e-6, glm::length(projectedU));
            const auto pxV = zoom_ * std::abs(dragUv_.scaleV) / std::max(1e-6, glm::length(projectedV));
            for (const auto vertex : active_->points) {
                const auto texU = glm::dot(vertex, dragUv_.uaxis) / dragUv_.scaleU + dragUv_.shiftU;
                const auto texV = glm::dot(vertex, dragUv_.vaxis) / dragUv_.scaleV + dragUv_.shiftV;
                const auto targetU = std::round((texU + du) / (w / 2.0)) * (w / 2.0);
                const auto targetV = std::round((texV + dv) / (h / 2.0)) * (h / 2.0);
                const auto errU = std::abs(texU + du - targetU) * pxU;
                const auto errV = std::abs(texV + dv - targetV) * pxV;
                if (errU < bestU) { bestU = errU; du = targetU - texU; }
                if (errV < bestV) { bestV = errV; dv = targetV - texV; }
            }
            if (bestU >= 8) du = std::round(du);
            if (bestV >= 8) dv = std::round(dv);
        }
        const auto stepU = du - lastAppliedU_, stepV = dv - lastAppliedV_;
        lastAppliedU_ = du; lastAppliedV_ = dv;
        applyGesture(editor::FaceGesture::Translate, stepU, stepV);
    } else if (drag_ == Drag::Rotate) {
        const auto d = point - toScreen(pivot_);
        auto angle = std::remainder(std::atan2(d.y(), d.x()) - startAngle_, 2 * M_PI);
        auto degrees = -glm::degrees(angle);
        if (!e->modifiers().testFlag(Qt::ShiftModifier)) degrees = std::round(degrees / 15.0) * 15.0;
        const auto increment = degrees - lastAngle_;
        lastAngle_ = degrees;
        applyGesture(editor::FaceGesture::Rotate, increment, 0);
    } else if (drag_ == Drag::ScaleU || drag_ == Drag::ScaleV) {
        const auto useU = drag_ == Drag::ScaleU;
        const auto& axis = useU ? dragUv_.uaxis : dragUv_.vaxis;
        const auto scale = useU ? dragUv_.scaleU : dragUv_.scaleV;
        const auto normal = active_->face.plane.normal;
        const auto projectedAxis = axis - normal * glm::dot(axis, normal);
        const auto axisLength = std::max(1e-9, glm::length(projectedAxis));
        const auto pivotCoord = glm::dot(pivot_, axis) / scale + (useU ? dragUv_.shiftU : dragUv_.shiftV);
        const auto oldDistance = std::max(1e-6, std::abs((edgeTexel_ - pivotCoord) * scale) / axisLength);
        auto cursorWorld = fromScreen(point);
        if (snap_) for (const auto vertex : active_->points)
            if (QLineF(point, toScreen(vertex)).length() <= 8) { cursorWorld = vertex; break; }
        const auto newDistance = std::abs(glm::dot(cursorWorld - pivot_, projectedAxis)) / axisLength;
        const auto factor = std::max(0.01, newDistance / oldDistance);
        const auto step = factor / lastScale_;
        const bool uniform = e->modifiers().testFlag(Qt::ControlModifier);
        lastScale_ = factor;
        applyGesture(editor::FaceGesture::Scale, drag_ == Drag::ScaleU || uniform ? step : 1,
                     drag_ == Drag::ScaleV || uniform ? step : 1);
    }
    lastPoint_ = point;
    update();
}
void UvView::finishDrag(bool cancel) {
    // Undo only our own entry: if history moved since the last write, the drag is already gone.
    if (cancel && changed_ && document_ && document_->historyState() == gestureState_) document_->undo();
    drag_ = Drag::None;
    changed_ = false;
    token_.clear();
    update();
}
void UvView::mouseReleaseEvent(QMouseEvent* e) {
    if ((e->button() == Qt::LeftButton && drag_ != Drag::None) ||
        (e->button() == Qt::MiddleButton && drag_ == Drag::Pan)) finishDrag(false);
}
void UvView::mouseDoubleClickEvent(QMouseEvent* e) {
    if (e->button() == Qt::MiddleButton) frameFace();
}
void UvView::wheelEvent(QWheelEvent* e) {
    const auto cursor = e->position();
    const auto world = (cursor - QPointF(width() / 2.0, height() / 2.0) - pan_) / zoom_;
    zoom_ = std::clamp(zoom_ * std::pow(1.2, e->angleDelta().y() / 120.0), 0.05, 100.0);
    pan_ = cursor - QPointF(width() / 2.0, height() / 2.0) - zoom_ * world;
    update();
}
void UvView::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Escape && drag_ != Drag::None) { finishDrag(true); e->accept(); return; }
    if (e->key() == Qt::Key_F) { frameFace(); e->accept(); return; }
    QWidget::keyPressEvent(e);
}
void UvView::leaveEvent(QEvent*) { unsetCursor(); }

} // namespace apex::ui
