#pragma once

#include "document/document.hpp"
#include "geometry/face_uv.hpp"

namespace apex::editor {

enum class FaceField { Material, ShiftU, ShiftV, ScaleU, ScaleV, Rotation, LightmapScale, SmoothingGroups };
enum class FaceAlignment { World, Face };
enum class FaceGesture { Translate, Rotate, Scale };
enum class FaceExtrapolation { Wrap, Projected, Match };
struct FaceApplyResult {
    bool applied = false;
    bool fellBackToWrap = false;
    operator bool() const { return applied; }
};
using FaceValue = std::variant<std::string, double, int, std::uint32_t>;
struct FaceProperties {
    std::size_t count = 0; // Zero distinguishes an empty selection from mixed fields.
    std::optional<std::string> material;
    std::optional<double> shiftU, shiftV, scaleU, scaleV, rotation;
    std::optional<int> lightmapScale;
    std::optional<std::uint32_t> smoothingGroups;
};
struct FaceSnapshot {
    geometry::Face face;
    std::vector<DVec3> points;
    kv1::Node keys;
};
// Owned snapshots, safe across transactions. Missing or invalid geometry returns nullopt.
std::optional<FaceSnapshot> faceSnapshot(const document::Document& doc, document::SideId side);
// D15: the `rotation` text to write after a side's axes change, or nullopt when `stored` already agrees
// or the U axis has no in-plane direction. Nothing reads `rotation` to compute UVs.
std::optional<std::string> derivedRotationText(const geometry::FaceUv& uv, DVec3 normal, const kv1::Node* stored);
FaceProperties summarize(const document::Document& doc, std::span<const document::SideId> sides);

} // namespace apex::editor
