#include "viewport/pick_math.hpp"

#include <algorithm>
#include <stdexcept>

namespace apex::viewport {
namespace {
DMat4 viewProjection(const Camera& camera, double aspect, double width, double height) {
    if (!(width > 0 && height > 0 && aspect > 0))
        throw std::invalid_argument("Picking requires positive viewport dimensions and aspect");
    return DMat4(camera.projectionMatrix(aspect)) * DMat4(camera.viewMatrix());
}
}

Ray pixelRay(const Camera& camera, double aspect, double x, double y, double width, double height) {
    const auto inverse = glm::inverse(viewProjection(camera, aspect, width, height));
    const auto near = inverse * DVec4{2 * x / width - 1, 2 * y / height - 1, 1, 1};
    const DVec3 relative = DVec3(near) / near.w;
    if (camera.isOrtho()) {
        const auto direction = inverse * DVec4{0, 0, -1, 0};
        return {camera.position + relative, glm::normalize(DVec3(direction))};
    }
    return {camera.position, glm::normalize(relative)};
}

std::array<Plane, 4> rectFrustum(const Camera& camera, double aspect,
                               double x0, double y0, double x1, double y1,
                               double width, double height) {
    const auto matrix = glm::transpose(viewProjection(camera, aspect, width, height));
    const double left = 2 * std::min(x0, x1) / width - 1;
    const double right = 2 * std::max(x0, x1) / width - 1;
    const double top = 2 * std::min(y0, y1) / height - 1;
    const double bottom = 2 * std::max(y0, y1) / height - 1;
    const std::array<DVec4, 4> equations = {
        left * matrix[3] - matrix[0], matrix[0] - right * matrix[3],
        top * matrix[3] - matrix[1], matrix[1] - bottom * matrix[3]};
    std::array<Plane, 4> planes;
    for (std::size_t i = 0; i < planes.size(); ++i) {
        const auto equation = equations[i] / glm::length(DVec3(equations[i]));
        const DVec3 normal(equation);
        planes[i] = {normal, glm::dot(normal, camera.position) - equation.w};
    }
    return planes;
}

} // namespace apex::viewport
