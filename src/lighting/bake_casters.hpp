#pragma once

#include "lighting/bake_scene.hpp"

namespace apex::lighting {

// World and func_detail brush snapshot; props, displacements and other brush entities are omitted.
// Sky/nodraw still block rays. Other tools/ faces are omitted. Each bake fragment is triangulated
// separately, so faceOf is exact even when chartFace subdivides a source side. faces must come
// from this document, with the same material lookup, and remain in the supplied order.
Result<BakeOcclusion> bakeCastersFromDocument(
    const document::Document& document, std::span<const BakeFace> faces,
    const std::function<BakeMaterial(std::string_view)>& material = {});

} // namespace apex::lighting
