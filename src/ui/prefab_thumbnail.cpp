#include "ui/prefab_thumbnail.hpp"

#include <QPainter>
#include <QPolygonF>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace apex::ui {
namespace {
constexpr double kYaw = 135.0, kPitch = 25.0;
}

QImage renderPrefabThumbnail(const editor::FragmentShape& shape, int size) {
    if (shape.bounds.empty() || size < 8) return {};
    const double yaw = kYaw * std::numbers::pi / 180.0, pitch = kPitch * std::numbers::pi / 180.0;
    const DVec3 toEye{std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch)};
    const DVec3 right = glm::normalize(glm::cross(-toEye, DVec3{0, 0, 1}));
    const DVec3 up = glm::cross(right, -toEye);
    const DVec3 centre = shape.bounds.center();
    const auto project = [&](DVec3 point) { return DVec2{glm::dot(point - centre, right), glm::dot(point - centre, up)}; };

    struct Face { QPolygonF polygon; double depth; double light; };
    std::vector<Face> faces;
    DVec2 low{1e300}, high{-1e300};
    const auto grow = [&](DVec2 p) { low = glm::min(low, p); high = glm::max(high, p); };
    const DVec3 lightDir = glm::normalize(toEye + DVec3{0.35, -0.2, 0.5});
    for (const auto& polygon : shape.polygons) {
        DVec3 normal{0.0}, mean{0.0};
        for (std::size_t i = 0; i < polygon.size(); ++i) {
            const auto& a = polygon[i];
            const auto& b = polygon[(i + 1) % polygon.size()];
            normal += glm::cross(a, b); // Newell.
            mean += a;
        }
        mean /= static_cast<double>(polygon.size());
        const double length = glm::length(normal);
        if (length < 1e-9) continue;
        normal /= length;
        if (glm::dot(normal, toEye) <= 0.0) continue;
        Face face;
        for (const auto& v : polygon) {
            const auto p = project(v);
            grow(p);
            face.polygon << QPointF(p.x, -p.y);
        }
        face.depth = glm::dot(mean - centre, toEye);
        face.light = std::clamp(glm::dot(normal, lightDir), 0.0, 1.0);
        faces.push_back(std::move(face));
    }
    // The wire box (no solids) and the framing both use the bounds' corners.
    std::vector<DVec2> box;
    for (int i = 0; i < 8; ++i) {
        const DVec3 corner{(i & 1) ? shape.bounds.max.x : shape.bounds.min.x, (i & 2) ? shape.bounds.max.y : shape.bounds.min.y,
                           (i & 4) ? shape.bounds.max.z : shape.bounds.min.z};
        box.push_back(project(corner));
        grow(box.back());
    }
    const double extent = std::max({high.x - low.x, high.y - low.y, 1e-6});
    const double scale = size * 0.9 / extent;
    const DVec2 middle = (low + high) * 0.5;
    const auto toImage = [&](QPointF p) { return QPointF(size * 0.5 + (p.x() - middle.x) * scale,
                                                         size * 0.5 + (p.y() + middle.y) * scale); };

    QImage image(size, size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    if (faces.empty()) {
        painter.setPen(QPen(QColor(150, 160, 175), 1.5));
        for (int i = 0; i < 8; ++i) for (const int bit : {1, 2, 4}) {
            if (i & bit) continue;
            const auto a = toImage({box[static_cast<std::size_t>(i)].x, -box[static_cast<std::size_t>(i)].y});
            const auto b = toImage({box[static_cast<std::size_t>(i | bit)].x, -box[static_cast<std::size_t>(i | bit)].y});
            painter.drawLine(a, b);
        }
        return image;
    }
    std::ranges::sort(faces, {}, &Face::depth);
    painter.setPen(QPen(QColor(40, 44, 52, 200), 1.0));
    for (const auto& face : faces) {
        const int shade = static_cast<int>(70 + 150 * face.light);
        painter.setBrush(QColor(shade, shade + 4, shade + 12));
        QPolygonF mapped;
        for (const auto& p : face.polygon) mapped << toImage(p);
        painter.drawPolygon(mapped);
    }
    return image;
}

} // namespace apex::ui
