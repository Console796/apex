// OBJ import: an apex.io/1 importer that turns each convex OBJ group into one solid.
// It shows how a plugin
//   - registers an importer for file extensions (registerImporter, in activate),
//   - receives a host file path and does its own file reading (plain paths until the 0.9.6 VFS),
//   - edits through the apex_import_context: the whole import is ONE undo step, and returning a
//     non-OK status (or throwing) rolls every solid back,
//   - reports problems to the user through Importer::report.
// Each `o` / `g` group is treated as a convex part: its face planes become the solid's planes. A group
// that is not closed and convex makes the host refuse it, which fails the import as a whole.
#include <apex/plugin.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {
const apex_host* host = nullptr;
using Vec = apex_vec3;

Vec sub(Vec a, Vec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
double dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

struct Group {
    std::string name;
    std::string material;
    std::vector<std::vector<size_t>> faces;   // indices into the shared vertex list
};

// Resolves an OBJ vertex reference ("3", "3/1", "3//2", "-1") to a zero-based index.
bool resolve(const std::string& token, size_t count, size_t& out) {
    char* end = nullptr;
    const long raw = std::strtol(token.c_str(), &end, 10);
    if (end == token.c_str() || raw == 0) return false;
    const long index = raw > 0 ? raw - 1 : static_cast<long>(count) + raw;
    if (index < 0 || index >= static_cast<long>(count)) return false;
    out = static_cast<size_t>(index);
    return true;
}

// Newell's method; robust for slightly non-planar polygons.
bool facePlane(const std::vector<Vec>& v, const std::vector<size_t>& face, apex_plane& plane, Vec& through) {
    Vec n{0, 0, 0};
    for (size_t i = 0; i < face.size(); ++i) {
        const Vec a = v[face[i]], b = v[face[(i + 1) % face.size()]];
        n.x += (a.y - b.y) * (a.z + b.z);
        n.y += (a.z - b.z) * (a.x + b.x);
        n.z += (a.x - b.x) * (a.y + b.y);
    }
    const double length = std::sqrt(dot(n, n));
    if (length < 1e-9) return false;
    n = {n.x / length, n.y / length, n.z / length};
    through = {0, 0, 0};
    for (const auto i : face) { through.x += v[i].x; through.y += v[i].y; through.z += v[i].z; }
    through = {through.x / face.size(), through.y / face.size(), through.z / face.size()};
    plane = {n, dot(n, through)};
    return true;
}

class ObjImporter final : public apex::plugin::Importer {
public:
    apex_status import(std::string_view path, const apex_import_context& context) override {
        const std::string filePath(path);
        std::ifstream file(filePath);
        if (!file) return fail(context, "cannot open " + std::string(path));
        std::vector<Vec> vertices;
        std::vector<Group> groups(1);
        bool grouped = false;
        std::string line;
        while (std::getline(file, line)) {
            std::istringstream in(line);
            std::string tag;
            in >> tag;
            if (tag == "v") {
                Vec p{};
                if (!(in >> p.x >> p.y >> p.z)) return fail(context, "bad vertex line: " + line);
                vertices.push_back(p);
            } else if (tag == "o" || tag == "g") {
                std::string name;
                std::getline(in >> std::ws, name);
                if (!grouped && groups.back().faces.empty()) groups.back().name = name;
                else groups.push_back({name, {}, {}});
                grouped = true;
            } else if (tag == "usemtl") {
                std::string material;
                in >> material;
                if (groups.back().material.empty()) groups.back().material = material;
            } else if (tag == "f") {
                std::vector<size_t> face;
                std::string token;
                while (in >> token) {
                    size_t index = 0;
                    if (!resolve(token, vertices.size(), index)) return fail(context, "bad face reference: " + line);
                    face.push_back(index);
                }
                if (face.size() < 3) return fail(context, "face with fewer than 3 vertices");
                groups.back().faces.push_back(std::move(face));
            }
        }
        size_t created = 0;
        for (const auto& group : groups) {
            if (group.faces.empty()) continue;
            const auto status = importGroup(context, vertices, group);
            if (status != APEX_OK) return status;
            ++created;
        }
        if (!created) return fail(context, "the file contains no faces");
        apex::plugin::Importer::report(context, "imported " + std::to_string(created) + " solid(s)");
        return APEX_OK;
    }

private:
    static apex_status fail(const apex_import_context& context, const std::string& message) {
        apex::plugin::Importer::report(context, message);
        return APEX_ERR_INVALID;
    }
    static apex_status importGroup(const apex_import_context& context, const std::vector<Vec>& vertices, const Group& group) {
        Vec centre{0, 0, 0};
        size_t used = 0;
        for (const auto& face : group.faces)
            for (const auto i : face) { centre.x += vertices[i].x; centre.y += vertices[i].y; centre.z += vertices[i].z; ++used; }
        centre = {centre.x / used, centre.y / used, centre.z / used};
        std::vector<apex_plane> planes;
        for (const auto& face : group.faces) {
            apex_plane plane{};
            Vec through{};
            if (!facePlane(vertices, face, plane, through)) continue;
            // Winding is not trusted: the outward side is the one facing away from the group's centre.
            if (dot(plane.normal, centre) - plane.distance > 0) {
                plane.normal = {-plane.normal.x, -plane.normal.y, -plane.normal.z};
                plane.distance = -plane.distance;
            }
            const bool duplicate = std::ranges::any_of(planes, [&](const apex_plane& p) {
                return dot(p.normal, plane.normal) > 0.9999 && std::abs(p.distance - plane.distance) < 1e-4;
            });
            if (!duplicate) planes.push_back(plane);
        }
        const std::string label = group.name.empty() ? std::string("(unnamed group)") : group.name;
        if (planes.size() < 4 || planes.size() > 256) return fail(context, "group '" + label + "' is not a usable convex part");
        const auto material = group.material.empty() ? std::string("DEV/DEV_MEASUREGENERIC01") : group.material;
        const auto status = context.ops->create_solid(context.operation, context.world, planes.data(), planes.size(),
                                                      apex::plugin::string(material));
        if (status != APEX_OK) return fail(context, "group '" + label + "' is not closed and convex");
        return APEX_OK;
    }
};

ObjImporter importer;
apex_status activate(void*) {
    return apex::plugin::registerImporter(*host, "plugin.objimport.obj", "Wavefront OBJ (convex parts)", "obj", importer);
}
const apex_plugin plugin{sizeof(apex_plugin), APEX_PLUGIN_ABI_MAJOR, APEX_PLUGIN_ABI_MINOR, nullptr, activate, nullptr, nullptr, nullptr};
}
extern "C" APEX_PLUGIN_EXPORT const apex_plugin* apex_plugin_entry(const apex_host* h) { host = h; return &plugin; }
