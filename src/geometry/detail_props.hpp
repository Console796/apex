#pragma once

#include "core/math.hpp"
#include "core/result.hpp"
#include "geometry/displacement.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace apex::geometry {

// Detail props (plan 0.8.9 D16): what VBSP's detailobjects.cpp scatters on a displacement whose
// material sets "%detailtype". Parsing and placement follow the SDK 2013 source; the placement is
// checked against the dprp lump P2CE's Windows vbsp compiles (tests/fixtures/detail, written by
// tools/detail_probe.py). Values are floats because VBSP's are, so the random draws land alike.

enum class DetailPropType : std::uint8_t { Model = 0, Sprite = 1, ShapeCross = 2, ShapeTri = 3 };

// One entry of a detail.vbsp group, as ParseDetailGroup stores it.
struct DetailModel {
    std::string name;  // The entry's key.
    std::string model; // The .mdl for DetailPropType::Model; empty for sprites.
    DetailPropType type = DetailPropType::Model;
    float amount = 1.0f; // Cumulative through the group, divided by the total when that exceeds 1.
    float minCosAngle = -1.0f, maxCosAngle = -1.0f;
    bool upright = false;
    int orientation = 0; // 0 normal, 1 screen-aligned, 2 screen-aligned vertical.
    std::array<Vec2, 2> pos{Vec2{-10.0f, 20.0f}, Vec2{10.0f, 0.0f}}; // Sprite upper-left and lower-right.
    std::array<Vec2, 2> tex{};                                        // Their texcoords in detail/detailsprites.
    float randomScaleStdDev = 0.0f;
    std::uint8_t shapeSize = 0, shapeAngle = 0, sway = 0;
};

struct DetailGroup {
    float alpha = 1.0f;
    std::vector<DetailModel> models;
};

struct DetailType {
    std::string name;
    float density = 0.0f;
    std::vector<DetailGroup> groups; // Sorted by alpha, as VBSP inserts them.
};

struct DetailVbsp {
    std::vector<DetailType> types;
    const DetailType* find(std::string_view name) const; // Case-sensitive, as VBSP's symbol lookup.
};

// A detail.vbsp file: its first top-level block holds the types. A sprite whose "sprite" key does
// not have five numbers is an error, as in VBSP.
Result<DetailVbsp> parseDetailVbsp(std::string_view text);

struct DetailProp {
    Vec3 origin{0.0f};
    Vec3 angles{0.0f}; // Pitch, yaw, roll in degrees.
    std::uint16_t group = 0, model = 0; // Indices into the DetailType.
    float scale = 1.0f;
};

// EmitDetailObjectsOnDisplacementFace for one displacement: corners as displacementMesh takes them,
// seed the side ID (VBSP's hammerfaceid). Models are placed whatever their .mdl; VBSP drops those
// it can't load after drawing their randoms. The Gaussian sprite scale starts a fresh stream here,
// so it matches VBSP only for the first such face it compiles (VBSP keeps a cached Gaussian value
// across faces); every other field matches for any face.
Result<std::vector<DetailProp>> placeDetailProps(std::span<const DVec3> corners, const Displacement& disp,
                                                 const DetailType& type, int seed);

} // namespace apex::geometry
