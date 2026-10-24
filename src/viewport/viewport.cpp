#include "viewport/viewport.hpp"

#include <string_view>

namespace apex::viewport {

namespace {

Error bad(std::string message) {
    return Error{ErrorCode::ParseError, std::move(message)};
}

json::Value num(double v) { json::Value r; r.data = v; return r; }
json::Value str(std::string v) { json::Value r; r.data = std::move(v); return r; }
json::Value boolean(bool v) { json::Value r; r.data = v; return r; }
json::Value vec3Json(DVec3 v) {
    json::Value r;
    r.data = json::Value::Array{num(v.x), num(v.y), num(v.z)};
    return r;
}

const json::Value* member(const json::Value::Object& o, std::string_view key) {
    const auto it = o.find(key);
    return it == o.end() ? nullptr : &it->second;
}

Result<double> readNumber(const json::Value::Object& o, std::string_view key, double fallback) {
    const auto* v = member(o, key);
    if (!v) return fallback;
    if (const auto* d = v->get<double>()) return *d;
    return std::unexpected(bad("viewport: '" + std::string(key) + "' must be a number"));
}

Result<DVec3> readVec3(const json::Value::Object& o, std::string_view key, DVec3 fallback) {
    const auto* v = member(o, key);
    if (!v) return fallback;
    const auto* a = v->get<json::Value::Array>();
    if (!a || a->size() != 3) return std::unexpected(bad("viewport: '" + std::string(key) + "' must be [x,y,z]"));
    DVec3 r{};
    for (int i = 0; i < 3; ++i) {
        const auto* d = (*a)[static_cast<std::size_t>(i)].get<double>();
        if (!d) return std::unexpected(bad("viewport: '" + std::string(key) + "' must hold numbers"));
        r[i] = *d;
    }
    return r;
}

Result<bool> readBool(const json::Value::Object& o, std::string_view key, bool fallback) {
    const auto* v = member(o, key);
    if (!v) return fallback;
    if (const auto* b = v->get<bool>()) return *b;
    return std::unexpected(bad("viewport: '" + std::string(key) + "' must be a boolean"));
}

constexpr struct { DisplayMode mode; const char* name; } kModes[] = {
    {DisplayMode::Wireframe, "wireframe"}, {DisplayMode::Flat, "flat"},
    {DisplayMode::Textured, "textured"},   {DisplayMode::Lit, "lit"},
    {DisplayMode::Baked, "baked"}};

} // namespace

const char* nameOf(DisplayMode mode) {
    for (const auto& e : kModes) if (e.mode == mode) return e.name;
    return "textured";
}

const char* nameOf(Preset preset) {
    switch (preset) {
    case Preset::Top: return "Top";
    case Preset::Front: return "Front";
    case Preset::Side: return "Side";
    case Preset::Camera: return "Camera";
    }
    return "Camera";
}

Viewport makePreset(Preset preset, DVec3 focus, double orthoHeight) {
    Viewport vp;
    vp.overlays.grid = true;
    auto& cam = vp.camera;
    if (preset == Preset::Camera) {
        cam.projection = ProjectionKind::Perspective;
        cam.axis = ViewAxis::Free;
        cam.controller = ControllerKind::Fly;
        vp.mode = DisplayMode::Textured;
        const DVec3 offset{-256.0, -256.0, 160.0};
        cam.position = focus + offset;
        cam.angles = anglesFromForward(-offset, DVec3{0, 0, 1});
        cam.orbitDistance = glm::length(offset);
        return vp;
    }
    cam.projection = ProjectionKind::Orthographic;
    cam.orthoHeight = orthoHeight;
    cam.controller = ControllerKind::Pan2D;
    cam.position = focus;
    setAxis(cam, preset == Preset::Top ? ViewAxis::NegZ
                 : preset == Preset::Front ? ViewAxis::PosY : ViewAxis::NegX);
    vp.mode = DisplayMode::Wireframe;
    return vp;
}

void applyPreset(Viewport& viewport, Preset preset) {
    const bool wasOrtho = viewport.camera.isOrtho();
    const DVec3 focus = wasOrtho ? viewport.camera.position : viewport.camera.pivot();
    const double height = viewport.camera.orthoHeight;
    const double fov = viewport.camera.fovYDegrees;
    const bool grid = viewport.overlays.grid;
    viewport = makePreset(preset, focus, height);
    viewport.camera.fovYDegrees = fov;
    viewport.overlays.grid = grid || viewport.overlays.grid;
}

json::Value toJson(const Viewport& vp) {
    const auto& c = vp.camera;
    json::Value::Object cam;
    cam["projection"] = str(nameOf(c.projection));
    cam["fov"] = num(c.fovYDegrees);
    cam["orthoHeight"] = num(c.orthoHeight);
    cam["axis"] = str(nameOf(c.axis));
    cam["position"] = vec3Json(c.position);
    cam["angles"] = vec3Json(c.angles);
    cam["orbitDistance"] = num(c.orbitDistance);
    json::Value::Object ov;
    ov["grid"] = boolean(vp.overlays.grid);
    ov["entityHelpers"] = boolean(vp.overlays.entityHelpers);
    ov["bounds"] = boolean(vp.overlays.bounds);
    ov["normals"] = boolean(vp.overlays.normals);
    ov["nodrawFaces"] = boolean(vp.overlays.nodrawFaces);
    json::Value::Object root;
    root["camera"].data = std::move(cam);
    root["mode"] = str(nameOf(vp.mode));
    root["overlays"].data = std::move(ov);
    json::Value out;
    out.data = std::move(root);
    return out;
}

Result<Viewport> viewportFromJson(const json::Value& value) {
    const auto* root = value.get<json::Value::Object>();
    if (!root) return std::unexpected(bad("viewport: expected an object"));
    Viewport vp;
    if (const auto* cv = member(*root, "camera")) {
        const auto* c = cv->get<json::Value::Object>();
        if (!c) return std::unexpected(bad("viewport: 'camera' must be an object"));
        if (const auto* p = member(*c, "projection")) {
            const auto* s = p->get<std::string>();
            const auto kind = s ? projectionFromString(*s) : std::nullopt;
            if (!kind) return std::unexpected(bad("viewport: unknown projection"));
            vp.camera.projection = *kind;
        }
        if (const auto* a = member(*c, "axis")) {
            const auto* s = a->get<std::string>();
            const auto axis = s ? viewAxisFromString(*s) : std::nullopt;
            if (!axis) return std::unexpected(bad("viewport: unknown axis"));
            vp.camera.axis = *axis;
        }
        auto fov = readNumber(*c, "fov", vp.camera.fovYDegrees);
        auto height = readNumber(*c, "orthoHeight", vp.camera.orthoHeight);
        auto dist = readNumber(*c, "orbitDistance", vp.camera.orbitDistance);
        auto pos = readVec3(*c, "position", vp.camera.position);
        auto ang = readVec3(*c, "angles", vp.camera.angles);
        if (!fov) return std::unexpected(fov.error());
        if (!height) return std::unexpected(height.error());
        if (!dist) return std::unexpected(dist.error());
        if (!pos) return std::unexpected(pos.error());
        if (!ang) return std::unexpected(ang.error());
        if (*fov <= 1.0 || *fov >= 179.0) return std::unexpected(bad("viewport: fov out of range"));
        if (*height <= 0.0 || *dist <= 0.0) return std::unexpected(bad("viewport: non-positive extent"));
        vp.camera.fovYDegrees = *fov;
        vp.camera.orthoHeight = *height;
        vp.camera.orbitDistance = *dist;
        vp.camera.position = *pos;
        vp.camera.angles = *ang;
        vp.camera.controller =
            vp.camera.isOrtho() ? ControllerKind::Pan2D : ControllerKind::Fly;
    }
    if (const auto* m = member(*root, "mode")) {
        const auto* s = m->get<std::string>();
        bool found = false;
        if (s) {
            for (const auto& e : kModes) {
                if (*s == e.name) { vp.mode = e.mode; found = true; }
            }
        }
        if (!found) return std::unexpected(bad("viewport: unknown display mode"));
    }
    if (const auto* ov = member(*root, "overlays")) {
        const auto* o = ov->get<json::Value::Object>();
        if (!o) return std::unexpected(bad("viewport: 'overlays' must be an object"));
        struct Field { const char* key; bool* dst; };
        for (const Field& f : {Field{"grid", &vp.overlays.grid},
                               Field{"entityHelpers", &vp.overlays.entityHelpers},
                               Field{"bounds", &vp.overlays.bounds},
                               Field{"normals", &vp.overlays.normals},
                               Field{"nodrawFaces", &vp.overlays.nodrawFaces}}) {
            auto b = readBool(*o, f.key, *f.dst);
            if (!b) return std::unexpected(b.error());
            *f.dst = *b;
        }
    }
    return vp;
}

} // namespace apex::viewport
