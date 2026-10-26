#include "core/entity_helpers.hpp"

#include "core/strings.hpp"

#include <cmath>
#include <sstream>

namespace apex::assets {
namespace {
bool balanced(std::string_view text) {
    std::size_t depth = 0;
    bool quoted = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quoted && c == '\\' && i + 1 < text.size()) { ++i; continue; }
        if (c == '"') quoted = !quoted;
        else if (!quoted) {
            if (c == '(') ++depth;
            else if (c == ')') {
                if (depth == 0) return false;
                --depth;
            }
        }
    }
    return !quoted && depth == 0;
}
template <typename T> std::optional<std::array<T, 3>> vector3(std::string_view text) {
    std::istringstream stream{std::string(text)};
    std::array<T, 3> result{};
    if (!(stream >> result[0] >> result[1] >> result[2])) return {};
    stream >> std::ws;
    if (!stream.eof()) return {};
    for (auto value : result) {
        if (!std::isfinite(static_cast<double>(value))) return {};
    }
    return result;
}
template <typename T> T bounds(const std::vector<std::string>& args) {
    T result;
    if (args.size() == 2) {
        result.mins = vector3<double>(args[0]);
        result.maxs = vector3<double>(args[1]);
    } else if (args.size() == 1) {
        auto size = vector3<double>(args[0]);
        if (size) {
            result.mins = *size;
            result.maxs = *size;
            for (unsigned i = 0; i < 3; ++i) {
                (*result.mins)[i] = -(*size)[i] / 2;
                (*result.maxs)[i] = (*size)[i] / 2;
            }
        }
    }
    return result;
}
}
std::vector<std::string> splitHelperArguments(std::string_view text) {
    std::vector<std::string> result;
    if (trim(text).empty()) return result;
    std::size_t start = 0, depth = 0;
    bool quoted = false;
    const auto append = [&](std::size_t end) {
        auto arg = trim(text.substr(start, end - start));
        // Only strip quotes when the first quoted string spans the entire argument.
        if (arg.size() >= 2 && arg.front() == '"') {
            std::size_t i = 1;
            for (; i < arg.size(); ++i) {
                if (arg[i] == '\\' && i + 1 < arg.size()) ++i;
                else if (arg[i] == '"') break;
            }
            if (i == arg.size() - 1) arg = arg.substr(1, arg.size() - 2);
        }
        result.emplace_back(arg);
    };
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quoted && c == '\\' && i + 1 < text.size()) { ++i; continue; }
        if (c == '"') quoted = !quoted;
        else if (!quoted) {
            if (c == '(') ++depth;
            else if (c == ')' && depth > 0) --depth;
            else if (c == ',' && depth == 0) { append(i); start = i + 1; }
        }
    }
    append(text.size());
    return result;
}
TypedHelper parseHelper(const EntityHelper& helper) {
    const auto name = toLowerAscii(trim(helper.name));
    const auto args = balanced(helper.arguments) ? splitHelperArguments(helper.arguments) :
        std::vector<std::string>{};
    const auto arg = [&](std::size_t index) -> std::string {
        return index < args.size() ? args[index] : std::string{};
    };
    if (name == "sphere") return SphereHelper{arg(0), vector3<int>(arg(1))};
    if (name == "line") return LineHelper{vector3<int>(arg(0)), arg(1), arg(2), arg(3), arg(4)};
    if (name == "studio") return StudioHelper{arg(0)};
    if (name == "studioprop") return StudioPropHelper{arg(0)};
    if (name == "lightprop") return LightPropHelper{arg(0)};
    if (name == "iconsprite") return IconSpriteHelper{arg(0)};
    if (name == "wirebox") return WireboxHelper{arg(0), arg(1)};
    if (name == "frustum") {
        FrustumHelper result{arg(0), arg(1), arg(2), {}, vector3<int>(arg(3))};
        if (!result.colour) result.colourKey = arg(3);
        std::istringstream stream{arg(4)};
        double pitch = 1;
        if (stream >> pitch && std::isfinite(pitch)) {
            stream >> std::ws;
            if (stream.eof()) result.pitchScale = pitch;
        }
        return result;
    }
    if (name == "cylinder") return CylinderHelper{vector3<int>(arg(0)), arg(1), arg(2),
        arg(3), arg(4), arg(5), arg(6)};
    if (name == "orientedwidthheight" || name == "orientedwidthheighthalf")
        return OrientedWidthHeightHelper{arg(0), arg(1), name == "orientedwidthheighthalf"};
    if (name == "orientedboundingbox") return OrientedBoundingBoxHelper{arg(0), arg(1), arg(2)};
    if (name == "clusteredlight") return ClusteredLightHelper{arg(0)};
    if (name == "size") return bounds<SizeHelper>(args);
    if (name == "bbox") return bounds<BboxHelper>(args);
    if (name == "color") return ColorHelper{vector3<int>(arg(0))};
    if (name == "catapult") return CatapultHelper{};
    if (name == "keyframe") return KeyframeHelper{};
    if (name == "animator") return AnimatorHelper{};
    if (name == "sidelist") return SidelistHelper{};
    if (name == "quadbounds") return QuadboundsHelper{};
    if (name == "sweptplayerhull") return SweptPlayerHullHelper{};
    if (name == "lightcone") return LightconeHelper{};
    if (name == "worldtext") return WorldtextHelper{};
    if (name == "instance") return InstanceHelper{};
    if (name == "sprite") return SpriteHelper{};
    if (name == "decal") return DecalHelper{};
    if (name == "overlay") return OverlayHelper{};
    if (name == "overlay_transition") return OverlayTransitionHelper{};
    if (name == "light") return LightHelper{};
    return UnknownHelper{helper.name, helper.arguments};
}
} // namespace apex::assets
