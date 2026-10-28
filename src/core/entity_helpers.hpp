#pragma once

#include "core/entity_class.hpp"

#include <variant>

namespace apex::assets {

// Split only top-level commas; fully quoted arguments lose their outer quotes.
std::vector<std::string> splitHelperArguments(std::string_view text);

struct SphereHelper {
    std::string radiusKey;
    std::optional<std::array<int, 3>> colour;
};
// Each pair matches other[key] == self[value]. An absent end uses self's origin.
struct LineHelper {
    std::optional<std::array<int, 3>> colour;
    std::string startKey, startValue, endKey, endValue;
};
struct StudioHelper { std::string model; };
struct StudioPropHelper { std::string model; };
struct LightPropHelper { std::string model; };
struct IconSpriteHelper { std::string material; };
struct WireboxHelper { std::string minsKey, maxsKey; };
struct FrustumHelper {
    std::string fovKey, nearKey, farKey, colourKey;
    std::optional<std::array<int, 3>> colour;
    double pitchScale = 1;
};
struct CylinderHelper {
    std::optional<std::array<int, 3>> colour;
    std::string startKey, startValue, startRadiusKey, endKey, endValue, endRadiusKey;
};
struct OrientedWidthHeightHelper {
    std::string widthKey, heightKey;
    bool half = false;
};
struct OrientedBoundingBoxHelper { std::string widthKey, heightKey, depthKey; };
struct ClusteredLightHelper { std::string kind; };
struct SizeHelper { std::optional<std::array<double, 3>> mins, maxs; };
struct BboxHelper { std::optional<std::array<double, 3>> mins, maxs; };
struct ColorHelper { std::optional<std::array<int, 3>> colour; };
struct CatapultHelper {};
struct KeyframeHelper {};
struct AnimatorHelper {};
struct SidelistHelper {};
struct QuadboundsHelper {};
struct SweptPlayerHullHelper {};
struct LightconeHelper {};
struct WorldtextHelper {};
struct InstanceHelper {};
struct SpriteHelper {};
struct DecalHelper {};
struct OverlayHelper {};
struct OverlayTransitionHelper {};
struct LightHelper {};
struct UnknownHelper { std::string name, arguments; };

using TypedHelper = std::variant<UnknownHelper, SphereHelper, LineHelper, StudioHelper,
    StudioPropHelper, LightPropHelper, IconSpriteHelper, WireboxHelper, FrustumHelper,
    CylinderHelper, OrientedWidthHeightHelper, OrientedBoundingBoxHelper, ClusteredLightHelper,
    SizeHelper, BboxHelper, ColorHelper, CatapultHelper, KeyframeHelper, AnimatorHelper,
    SidelistHelper, QuadboundsHelper, SweptPlayerHullHelper, LightconeHelper, WorldtextHelper,
    InstanceHelper, SpriteHelper, DecalHelper, OverlayHelper, OverlayTransitionHelper, LightHelper>;

// Invalid or missing arguments leave the corresponding fields empty/defaulted.
TypedHelper parseHelper(const EntityHelper& helper);

} // namespace apex::assets
