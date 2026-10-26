#include "render/helper_shapes.hpp"

#include "core/entity_helpers.hpp"
#include "core/strings.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <numbers>
#include <sstream>
#include <type_traits>
#include <utility>

namespace apex::render {
namespace {

constexpr auto selected = HelperVisibility::WhenSelected;
constexpr auto always = HelperVisibility::Always;

bool finite(DVec3 p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}
bool finite(Vec4 c) {
    return std::isfinite(c.x) && std::isfinite(c.y) && std::isfinite(c.z) && std::isfinite(c.w);
}
std::optional<double> number(const kv1::Node& block, std::string_view key) {
    if (key.empty()) return {};
    if (const auto* node = block.find(key)) {
        if (const auto v = node->asDouble(); v && std::isfinite(*v)) return *v;
    }
    return {};
}
std::optional<DVec3> vector(const kv1::Node& block, std::string_view key) {
    if (key.empty()) return {};
    if (const auto* node = block.find(key)) {
        if (const auto v = node->asVec3()) {
            const DVec3 p{(*v)[0], (*v)[1], (*v)[2]};
            if (finite(p)) return p;
        }
    }
    return {};
}
std::optional<std::string> text(const kv1::Node& block, std::string_view key) {
    if (key.empty()) return {};
    if (const auto* node = block.find(key)) {
        if (auto v = node->decodedValue()) return *v;
    }
    return {};
}
Vec4 colour(const std::optional<std::array<int, 3>>& c, Vec4 fallback) {
    if (!c) return fallback;
    return {static_cast<float>(std::clamp((*c)[0], 0, 255)) / 255.0f,
            static_cast<float>(std::clamp((*c)[1], 0, 255)) / 255.0f,
            static_cast<float>(std::clamp((*c)[2], 0, 255)) / 255.0f, 1};
}
Vec4 keyColour(const kv1::Node& block, std::string_view key, Vec4 fallback) {
    const auto value = text(block, key);
    if (!value) return fallback;
    std::istringstream stream{*value};
    std::array<int, 3> rgb{};
    if (!(stream >> rgb[0] >> rgb[1] >> rgb[2])) return fallback;
    // FGD color255 keys can carry a fourth brightness/alpha component.
    stream >> std::ws;
    if (!stream.eof()) {
        double fourth = 0;
        if (!(stream >> fourth) || !std::isfinite(fourth)) return fallback;
        stream >> std::ws;
        if (!stream.eof()) return fallback;
    }
    return colour(rgb, fallback);
}
const assets::EntityProperty* property(const assets::EntityClass& cls, std::string_view name) {
    const auto it = std::ranges::find_if(cls.properties, [&](const auto& p) { return ciEquals(p.name, name); });
    return it == cls.properties.end() ? nullptr : &*it;
}
std::string chainKey(const assets::EntityClass& cls) {
    for (const auto name : {"nextkey", "target"}) {
        if (const auto* p = property(cls, name); p && ciEquals(p->type, "target_destination"))
            return p->name;
    }
    return {};
}

struct Builder {
    const HelperContext& c;
    HelperShapes shapes;
    bool invalid = false;

    DVec3 world(DVec3 p) const { return c.pose.origin + c.pose.basis * p; }
    void segment(DVec3 a, DVec3 b, Vec4 col, HelperVisibility visibility,
                 std::optional<document::NodeId> peer = {}, std::optional<document::NodeId> anchor = {}) {
        if (!finite(a) || !finite(b) || !finite(col)) { invalid = true; return; }
        shapes.segments.push_back({a, b, col, visibility, peer, anchor});
    }
    void ring(DVec3 center, DVec3 u, DVec3 v, double radius, int count, Vec4 col,
              HelperVisibility visibility, std::optional<document::NodeId> peer = {}) {
        const auto point = [&](int i) {
            const double angle = 2 * std::numbers::pi * static_cast<double>(i) / count;
            return center + radius * (u * std::cos(angle) + v * std::sin(angle));
        };
        for (int i = 0; i < count; ++i) segment(point(i), point((i + 1) % count), col, visibility, peer, peer);
    }
    void rectangle(const std::array<DVec3, 4>& p, Vec4 col, HelperVisibility visibility) {
        for (std::size_t i = 0; i < 4; ++i) segment(p[i], p[(i + 1) % 4], col, visibility);
    }
    void box(DVec3 mins, DVec3 maxs, DVec3 origin, const DMat3& basis, HelperVisibility visibility) {
        std::array<DVec3, 8> p{};
        for (unsigned i = 0; i < 8; ++i)
            p[i] = origin + basis * DVec3{(i & 1) ? maxs.x : mins.x,
                                         (i & 2) ? maxs.y : mins.y, (i & 4) ? maxs.z : mins.z};
        for (unsigned i = 0; i < 8; ++i)
            for (unsigned bit = 1; bit <= 4; bit *= 2)
                if (!(i & bit)) segment(p[i], p[i | bit], c.defaultColour, visibility);
    }
    std::vector<HelperPeer> named(std::string_view key) const {
        const auto name = text(c.block, key);
        if (!c.peersNamed || !name || trim(*name).empty()) return {};
        auto peers = c.peersNamed(*name);
        std::erase_if(peers, [](const auto& peer) { return !finite(peer.position); });
        return peers;
    }
    // LineHelper pairs mean other[key] == self[value]. The API exposes only targetname
    // lookups, so non-targetname pairs (e.g. lightingorigin) cannot be resolved here.
    struct Endpoint { DVec3 position; std::optional<document::NodeId> peer; };
    std::vector<Endpoint> endpoint(std::string_view key, std::string_view value) const {
        if (!ciEquals(key, "targetname")) return {};
        // FGD cylinders use (targetname,targetname) to designate self. The name
        // index excludes self, and an unnamed entity must still supply this endpoint.
        if (ciEquals(value, "targetname")) return {{c.pose.origin, {}}};
        std::vector<Endpoint> result;
        for (const auto& peer : named(value)) result.push_back({peer.position, peer.id});
        return result;
    }
    template <typename H, typename F> void connections(const H& h, F draw) {
        if (h.startKey.empty() || h.startValue.empty()) return;
        const auto start = endpoint(h.startKey, h.startValue);
        if (h.endKey.empty() && h.endValue.empty()) {
            for (const auto& peer : start) draw(c.pose.origin, std::nullopt, peer.position, peer.peer);
        } else if (!h.endKey.empty() && !h.endValue.empty()) {
            const auto end = endpoint(h.endKey, h.endValue);
            for (const auto& a : start)
                for (const auto& b : end) draw(a.position, a.peer, b.position, b.peer);
        }
    }
    void draw(const assets::SphereHelper& h) {
        const auto radius = number(c.block, h.radiusKey);
        if (!radius || *radius <= 0) return;
        const auto col = colour(h.colour, c.defaultColour);
        ring(c.pose.origin, {1, 0, 0}, {0, 1, 0}, *radius, 48, col, selected);
        ring(c.pose.origin, {1, 0, 0}, {0, 0, 1}, *radius, 48, col, selected);
        ring(c.pose.origin, {0, 1, 0}, {0, 0, 1}, *radius, 48, col, selected);
    }
    void draw(const assets::LineHelper& h) {
        connections(h, [&](DVec3 a, std::optional<document::NodeId> aPeer, DVec3 b, std::optional<document::NodeId> bPeer) {
            segment(a, b, colour(h.colour, c.defaultColour), HelperVisibility::WhenSelfOrPeerSelected, bPeer, aPeer);
        });
    }
    void draw(const assets::WireboxHelper& h) {
        const auto mins = vector(c.block, h.minsKey), maxs = vector(c.block, h.maxsKey);
        if (!mins || !maxs) return;
        // comp_propcombine_set describes minimum/maximum offsets for an AABB. Keep
        // these in world axes, offset by origin, independent of the entity's angles.
        box(glm::min(*mins, *maxs), glm::max(*mins, *maxs), c.pose.origin, DMat3{1}, always);
    }
    void draw(const assets::FrustumHelper& h) {
        const auto fov = number(c.block, h.fovKey), near = number(c.block, h.nearKey), far = number(c.block, h.farKey);
        if (!fov || !near || !far || *fov <= 0 || *fov >= 180 || *near < 0 || *far <= *near) return;
        auto angles = c.pose.anglesDeg;
        angles.x *= h.pitchScale;
        const auto axes = angleVectors(angles);
        const DMat3 basis{axes.forward, -axes.right, axes.up}; // Same columns as entityPose.
        const auto col = h.colour ? colour(h.colour, c.defaultColour) : keyColour(c.block, h.colourKey, c.defaultColour);
        const double slope = std::tan(glm::radians(*fov) / 2);
        const auto corners = [&](double d) {
            const double r = d * slope;
            return std::array<DVec3, 4>{c.pose.origin + basis * DVec3{d, -r, -r},
                c.pose.origin + basis * DVec3{d, r, -r}, c.pose.origin + basis * DVec3{d, r, r},
                c.pose.origin + basis * DVec3{d, -r, r}};
        };
        const auto a = corners(*near), b = corners(*far);
        rectangle(a, col, selected);
        rectangle(b, col, selected);
        for (std::size_t i = 0; i < 4; ++i) segment(a[i], b[i], col, selected);
    }
    void draw(const assets::CylinderHelper& h) {
        const auto r0 = number(c.block, h.startRadiusKey);
        const auto r1 = h.endRadiusKey.empty() ? r0 : number(c.block, h.endRadiusKey);
        if (!r0 || !r1 || *r0 < 0 || *r1 < 0 || (*r0 == 0 && *r1 == 0)) return;
        const auto col = colour(h.colour, c.defaultColour);
        connections(h, [&](DVec3 a, std::optional<document::NodeId> aPeer, DVec3 b, std::optional<document::NodeId> bPeer) {
            const auto delta = b - a;
            const double length = glm::length(delta);
            if (!std::isfinite(length) || length <= 0) return;
            const auto axis = delta / length;
            const auto u = glm::normalize(glm::cross(axis, std::abs(axis.z) < 0.9 ? DVec3{0, 0, 1} : DVec3{0, 1, 0}));
            const auto v = glm::cross(axis, u);
            ring(a, u, v, *r0, 24, col, selected, aPeer);
            ring(b, u, v, *r1, 24, col, selected, bPeer);
            for (const auto offset : {u, v, -u, -v})
                segment(a + offset * *r0, b + offset * *r1, col, selected, bPeer, aPeer);
        });
    }
    void draw(const assets::LightconeHelper&) {
        // BaseSpotLight's _cone is the outer half-angle; light_dynamic uses distance.
        const auto angle = number(c.block, "_cone");
        if (!angle || *angle <= 0 || *angle >= 90) return;
        std::string_view distanceKey = "_distance";
        if (property(c.cls, "distance")) distanceKey = "distance";
        else if (!c.block.find(distanceKey) && c.block.find("_zero_percent_distance")) distanceKey = "_zero_percent_distance";
        const auto distance = c.block.find(distanceKey) ? number(c.block, distanceKey) : std::optional<double>{256};
        if (!distance || *distance <= 0) return;
        const double radius = *distance * std::tan(glm::radians(*angle));
        const auto center = world({*distance, 0, 0});
        const auto u = c.pose.basis[1], v = c.pose.basis[2];
        ring(center, u, v, radius, 24, c.defaultColour, selected);
        for (const auto offset : {u, v, -u, -v}) segment(c.pose.origin, center + radius * offset, c.defaultColour, selected);
    }
    void draw(const assets::OrientedWidthHeightHelper& h) {
        const auto w = number(c.block, h.widthKey), height = number(c.block, h.heightKey);
        if (!w || !height || *w <= 0 || *height <= 0) return;
        const double x = *w * (h.half ? 1 : 0.5), y = *height * (h.half ? 1 : 0.5);
        rectangle({world({0, -x, -y}), world({0, x, -y}), world({0, x, y}), world({0, -x, y})}, c.defaultColour, selected);
    }
    void draw(const assets::OrientedBoundingBoxHelper& h) {
        const auto w = number(c.block, h.widthKey), height = number(c.block, h.heightKey), d = number(c.block, h.depthKey);
        if (!w || !height || !d || *w <= 0 || *height <= 0 || *d <= 0) return;
        const DVec3 half{*d / 2, *w / 2, *height / 2};
        box(-half, half, c.pose.origin, c.pose.basis, selected);
    }
    void draw(const assets::QuadboundsHelper&) {
        // func_breakable_surf stores Hammer-generated vecline corners in world space.
        const auto ll = vector(c.block, "lowerleft"), lr = vector(c.block, "lowerright"),
                   ul = vector(c.block, "upperleft"), ur = vector(c.block, "upperright");
        if (!ll || !lr || !ul || !ur) return;
        rectangle({*ll, *lr, *ur, *ul}, c.defaultColour, selected);
        segment(*ll, *ur, c.defaultColour, selected);
        segment(*lr, *ul, c.defaultColour, selected);
    }
    void draw(const assets::SweptPlayerHullHelper&) {
        // func_useableladder declares point0/point1, no target_destination. The requested
        // player hull stays upright in world axes, including for angled entities.
        box({-16, -16, 0}, {16, 16, 72}, c.pose.origin, DMat3{1}, selected);
        const auto key = chainKey(c.cls);
        for (const auto& peer : named(key)) box({-16, -16, 0}, {16, 16, 72}, peer.position, DMat3{1}, selected);
    }
    void parabola(DVec3 velocity, double duration, std::optional<document::NodeId> peer) {
        if (!finite(velocity) || !std::isfinite(duration) || duration <= 0) return;
        const auto point = [&](int i) {
            const double t = duration * i / 32;
            return c.pose.origin + velocity * t - DVec3{0, 0, 300 * t * t};
        };
        for (int i = 0; i < 32; ++i) segment(point(i), point(i + 1), c.defaultColour, selected, peer);
    }
    void draw(const assets::CatapultHelper&) {
        auto speed = number(c.block, "playerSpeed");
        if (!c.block.find("playerSpeed")) speed = number(c.block, "physicsSpeed");
        if (!speed || *speed <= 0) return;
        const auto target = text(c.block, "launchTarget");
        if (target && !trim(*target).empty()) {
            for (const auto& peer : named("launchTarget")) {
                const auto delta = peer.position - c.pose.origin;
                const double distance = glm::length(delta);
                if (!std::isfinite(distance) || distance <= 0) continue;
                // With g=600, |delta/t + (0,0,g*t/2)|^2=v^2. The low arc uses
                // t^2=2*|delta|^2/(v^2-g*dz+sqrt((v^2-g*dz)^2-g^2*|delta|^2)).
                // Unreachable: v_min^2=g*(dz+|delta|), t^2=2*|delta|/g.
                const double q = *speed * *speed - 600 * delta.z;
                const double discriminant = q * q - 360000 * distance * distance;
                const double t = q > 0 && discriminant >= 0
                    ? std::sqrt(2 * distance * distance / (q + std::sqrt(discriminant)))
                    : std::sqrt(2 * distance / 600);
                parabola(delta / t + DVec3{0, 0, 300 * t}, t, peer.id);
            }
        } else if (const auto angles = vector(c.block, "launchDirection")) {
            const auto velocity = angleVectors(*angles).forward * *speed;
            const double untilFloor = (velocity.z + std::sqrt(velocity.z * velocity.z + 1200 * 4096)) / 600;
            parabola(velocity, std::min(5.0, untilFloor), {});
        }
    }
    void chain() {
        for (const auto& peer : named(chainKey(c.cls))) segment(c.pose.origin, peer.position, c.defaultColour, always, peer.id);
    }
    void draw(const assets::KeyframeHelper&) { chain(); }
    void draw(const assets::AnimatorHelper&) { chain(); }
    void draw(const assets::SidelistHelper&) {
        for (const auto& p : c.cls.properties) {
            if (!ciEquals(p.type, "sidelist")) continue;
            const auto value = text(c.block, p.name);
            if (!value) continue;
            std::istringstream stream{*value};
            std::string token;
            while (stream >> token) {
                int id = 0;
                std::string_view view{token};
                if (view.starts_with('+')) view.remove_prefix(1);
                const auto [end, error] = std::from_chars(view.data(), view.data() + view.size(), id);
                if (error == std::errc{} && end == view.data() + view.size() &&
                    std::ranges::find(shapes.sideIds, id) == shapes.sideIds.end()) shapes.sideIds.push_back(id);
            }
        }
    }
    void draw(const assets::WorldtextHelper&) {
        std::string_view key = "message";
        for (const auto candidate : {"message", "text", "textstring"})
            if (property(c.cls, candidate)) { key = candidate; break; }
        const auto message = text(c.block, key).value_or(text(c.block, "classname").value_or(c.cls.name));
        const auto col = keyColour(c.block, "color", c.defaultColour);
        if (finite(c.pose.origin) && finite(col)) shapes.labels.push_back({c.pose.origin, message, col});
    }
    template <typename H> void draw(const H&) {
        // Models, sprites, entity boxes, colour and instances are handled by other paths.
    }
};

} // namespace

HelperShapes buildHelperShapes(const HelperContext& context) {
    Builder builder{context, {}};
    for (const auto& helper : context.cls.helpers) {
        const auto before = builder.shapes.segments.size();
        builder.invalid = false;
        std::visit([&](const auto& h) {
            using H = std::decay_t<decltype(h)>;
            if constexpr (std::is_same_v<H, assets::UnknownHelper> || std::is_same_v<H, assets::LightHelper> ||
                          std::is_same_v<H, assets::ClusteredLightHelper> || std::is_same_v<H, assets::DecalHelper> ||
                          std::is_same_v<H, assets::OverlayHelper> || std::is_same_v<H, assets::OverlayTransitionHelper>) {
                const auto name = toLowerAscii(trim(helper.name));
                if (std::ranges::find(builder.shapes.unsupported, name) == builder.shapes.unsupported.end())
                    builder.shapes.unsupported.push_back(name);
            } else builder.draw(h);
        }, assets::parseHelper(helper));
        if (builder.invalid) builder.shapes.segments.resize(before);
    }
    return std::move(builder.shapes);
}

} // namespace apex::render
