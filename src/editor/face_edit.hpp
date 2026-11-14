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

// Plan 0.8.2: the read-only Displacement section of Face Properties. A field is nullopt when the
// readable displaced faces disagree (or none is readable); alphas are as written, not clamped.
struct DisplacementSummary {
    std::size_t displaced = 0; // Selected faces that carry a dispinfo.
    std::size_t readable = 0;  // Of those, the ones that parse.
    std::optional<int> power, flags;
    int flagsAll = 0, flagsAny = 0;           // Bits set on every / some readable displaced face.
    std::optional<double> elevation;
    std::optional<double> alphaMin, alphaMax; // Over every readable displaced face.
    std::vector<std::string> problems;        // map_problems' texts for the selected faces.
    int gridSize() const { return power ? (1 << *power) + 1 : 0; }
};
DisplacementSummary summarizeDisplacements(const document::Document& doc, std::span<const document::SideId> sides);
// Hammer's dispinfo "flags" bits (geometry::Displacement::kFlag*). Other set bits are named by
// value, e.g. "0x1", so nothing is hidden.
std::vector<std::string> displacementFlagNames(int flags);

} // namespace apex::editor
