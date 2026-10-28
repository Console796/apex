#pragma once

#include <QImage>

#include "editor/clipboard.hpp"

namespace apex::ui {

// A prefab's thumbnail, drawn on the CPU: its solids as flat-shaded polygons (back faces culled,
// painter's order) from the model thumbnail's camera (yaw 135, pitch 25, orthographic, 10% padding),
// or its bounds as a wire box when it holds no solids. It is not a textured render. Transparent
// background, `size` square. An empty shape gives a null image.
QImage renderPrefabThumbnail(const editor::FragmentShape& shape, int size);

} // namespace apex::ui
