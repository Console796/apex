#include "lighting/bsp_lightmaps.hpp"

#include "document/document.hpp"
#include "geometry/solid_geometry.hpp"

#include <bsppp/bsppp.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

namespace apex::lighting {
namespace {

constexpr int kSurfSky = 0x4, kSurfNoLight = 0x400;
// A fragment's plane may differ from its side's by float noise only; anything else is another surface.
constexpr double kNormalAgreement = 0.9999, kPlaneTolerance = 0.25;
// Slivers a fragment shares with a neighbouring side only along their common edge are not pieces.
constexpr double kMinPieceArea = 0.5;

std::unexpected<Error> fail(std::string message) {
    return std::unexpected(Error{ErrorCode::ParseError, std::move(message)});
}

template <typename V> DVec3 vec(const V& v) { return {v[0], v[1], v[2]}; }

DVec3 newell(std::span<const DVec3> loop) {
    DVec3 sum{0.0};
    for (std::size_t i = 0; i < loop.size(); ++i) {
        const auto& a = loop[i];
        const auto& b = loop[(i + 1) % loop.size()];
        sum += DVec3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y));
    }
    return sum;
}

// The part of `loop` that lies inside the side's polygon, when `loop` lies on the side's plane (empty
// otherwise). Convex clip against the side's edges; the loop is a convex BSP face.
std::vector<DVec3> clipToSide(const SideOwner& side, std::span<const DVec3> loop) {
    const auto& n = side.plane.normal;
    for (const auto& v : loop)
        if (std::abs(glm::dot(n, v) - side.plane.dist) > kPlaneTolerance) return {};
    std::vector<DVec3> out(loop.begin(), loop.end());
    for (std::size_t i = 0; i < side.loop.size() && !out.empty(); ++i) {
        const auto& a = side.loop[i];
        const auto inward = glm::cross(n, side.loop[(i + 1) % side.loop.size()] - a);
        const double length = glm::length(inward);
        if (length <= 0) continue;
        const auto distance = [&](const DVec3& v) { return glm::dot(inward, v - a) / length; };
        std::vector<DVec3> next;
        for (std::size_t k = 0; k < out.size(); ++k) {
            const auto& p = out[k];
            const auto& q = out[(k + 1) % out.size()];
            const double dp = distance(p), dq = distance(q);
            if (dp >= 0) next.push_back(p);
            if ((dp >= 0) != (dq >= 0)) next.push_back(p + (q - p) * (dp / (dp - dq)));
        }
        out = std::move(next);
    }
    return out;
}

} // namespace

Result<std::vector<BspFragment>> readBspFragments(const std::filesystem::path& path) {
    bsppp::BSP bsp(path.string());
    if (!bsp) return fail("cannot open " + path.string());
    if (bsp.getVersion() != 25) return fail("BSP version " + std::to_string(bsp.getVersion()) + ", expected 25");
    const auto hdrFaces = bsp.getLumpData(bsppp::BSPLump::FACES_HDR);
    const auto lighting = bsp.getLumpData(bsppp::BSPLump::LIGHTING_HDR);
    static_assert(sizeof(bsppp::BSPFace) == 72, "dface_t is 72 bytes in BSP v25");
    if (!hdrFaces || hdrFaces->empty() || hdrFaces->size() % sizeof(bsppp::BSPFace))
        return fail("BSP has no HDR face lump");
    if (!lighting || lighting->empty()) return fail("BSP has no HDR lighting lump");
    const auto planes = bsp.getLumpData<bsppp::BSPLump::PLANES>();
    const auto texinfo = bsp.getLumpData<bsppp::BSPLump::TEXINFO>();
    const auto vertices = bsp.getLumpData<bsppp::BSPLump::VERTEXES>();
    const auto edges = bsp.getLumpData<bsppp::BSPLump::EDGES>();
    const auto surfEdges = bsp.getLumpData<bsppp::BSPLump::SURFEDGES>();
    const auto ids = bsp.getLumpData(bsppp::BSPLump::FACEIDS);
    const std::size_t count = hdrFaces->size() / sizeof(bsppp::BSPFace);
    // FACEIDS is an optional uint16 per emitted face, parallel to FACES and FACES_HDR.
    const bool hasIds = ids && ids->size() == count * sizeof(std::uint16_t);

    std::vector<BspFragment> result;
    for (std::size_t i = 0; i < count; ++i) {
        bsppp::BSPFace face;
        std::memcpy(&face, hdrFaces->data() + i * sizeof(face), sizeof(face));
        if (face.texInfo < 0 || std::size_t(face.texInfo) >= texinfo.size() || face.numEdges < 3 ||
            face.planeNum >= planes.size()) continue;
        const auto& info = texinfo[std::size_t(face.texInfo)];
        if (info.flags & kSurfSky) continue;
        BspFragment fragment;
        if (hasIds) std::memcpy(&fragment.hammerId, ids->data() + i * sizeof(std::uint16_t), sizeof(std::uint16_t));
        fragment.displacement = face.dispInfo != -1;
        const auto& plane = planes[face.planeNum];
        // `side` orients the face against its BSP node, not the light: the face plane is already the lit one.
        fragment.plane = Plane{vec(plane.normal), plane.dist};
        for (int e = 0; e < face.numEdges; ++e) {
            const auto index = std::size_t(face.firstEdge + e);
            if (face.firstEdge < 0 || index >= surfEdges.size()) return fail("face edge out of range");
            const auto se = surfEdges[index].surfEdge;
            const auto edge = std::size_t(se < 0 ? -std::int64_t(se) : se);
            if (edge >= edges.size()) return fail("surfedge out of range");
            const auto v = se >= 0 ? edges[edge].v0 : edges[edge].v1;
            if (v >= vertices.size()) return fail("vertex out of range");
            fragment.vertices.push_back(vec(vertices[v].position));
        }
        const auto& s = info.lightmapVector1;
        const auto& t = info.lightmapVector2;
        fragment.lightmapVectors = {DVec4(s[0], s[1], s[2], s[3]), DVec4(t[0], t[1], t[2], t[3])};
        fragment.mins = {face.lightmapTextureMinsInLuxels[0], face.lightmapTextureMinsInLuxels[1]};
        fragment.extents = {face.lightmapTextureSizeInLuxels[0], face.lightmapTextureSizeInLuxels[1]};
        // Style 0 only. A bumped face stores the flat set first (then three bump sets); NOLIGHT faces
        // keep no lightmap even when VRAD gave them an offset.
        const bool lit = face.lightOffset >= 0 && face.styles[0] == 0 && !(info.flags & kSurfNoLight) &&
                         face.lightmapTextureSizeInLuxels[0] >= 0 && face.lightmapTextureSizeInLuxels[1] >= 0 &&
                         face.lightmapTextureSizeInLuxels[0] < 1 << 12 && face.lightmapTextureSizeInLuxels[1] < 1 << 12;
        if (lit) {
            const std::size_t luxels = std::size_t(fragment.extents[0] + 1) * std::size_t(fragment.extents[1] + 1);
            if (std::size_t(face.lightOffset) + luxels * 4 > lighting->size()) return fail("lightmap out of range");
            fragment.luxels.reserve(luxels);
            for (std::size_t k = 0; k < luxels; ++k) {
                const auto* p = reinterpret_cast<const unsigned char*>(lighting->data()) + face.lightOffset + 4 * k;
                // ColorRGBExp32: three mantissas and a signed exponent; (m + 0.5) 2^e / 255 as light_parity.py measured.
                const double scale = std::ldexp(1.0, static_cast<signed char>(p[3])) / 255.0;
                fragment.luxels.push_back(p[0] == 0 && p[1] == 0 && p[2] == 0 ? Vec3{0.0f} :
                    Vec3(float((p[0] + 0.5) * scale), float((p[1] + 0.5) * scale), float((p[2] + 0.5) * scale)));
            }
        }
        result.push_back(std::move(fragment));
    }
    return result;
}

SideOwners sideOwners(const document::Document& document) {
    SideOwners owners;
    const auto walk = [&](const auto& self, document::NodeId id) -> void {
        const auto handle = document.handle(id);
        if (!handle) return;
        if (const auto* solid = std::get_if<Handle<document::Solid>>(&*handle)) {
            const auto* node = document.get(*solid);
            if (auto shape = geometry::solidGeometry(*node->block)) {
                std::set<std::uint32_t> displacements;
                for (const auto* side : node->block->findAll("side"))
                    if (const auto* sideId = side->find("id"); sideId && side->find("dispinfo"))
                        if (const auto value = sideId->asInt()) displacements.insert(std::uint32_t(*value));
                for (const auto& face : shape->poly.faces) {
                    if (displacements.contains(face.sideId)) continue;
                    SideOwner owner{node->id, face.sideId, face.plane, {}};
                    for (const auto index : face.vertices) owner.loop.push_back(shape->poly.vertices[index]);
                    owners.push_back(std::move(owner));
                }
            }
        }
        for (const auto child : document.get(*handle)->children) self(self, child);
    };
    for (const auto root : document.roots()) walk(walk, root);
    return owners;
}

BspLightmaps mapBspFragments(std::span<const BspFragment> fragments, const SideOwners& owners) {
    BspLightmaps result;
    std::set<std::pair<std::uint64_t, std::uint32_t>> sides;
    for (const auto& fragment : fragments) {
        ++result.stats.fragments;
        if (fragment.displacement) { ++result.stats.displacement; continue; }
        if (fragment.luxels.empty()) { ++result.stats.noData; continue; }
        if (fragment.vertices.size() < 3) { ++result.stats.mismatched; continue; }
        BakeFace chartFace;
        try {
            chartFace.chart = bspChart({DVec3(fragment.lightmapVectors[0]), DVec3(fragment.lightmapVectors[1])},
                {fragment.lightmapVectors[0].w, fragment.lightmapVectors[1].w}, fragment.mins, fragment.extents,
                fragment.plane);
        } catch (const std::invalid_argument&) { ++result.stats.mismatched; continue; }
        std::size_t pieces = 0;
        for (const auto& side : owners) {
            if (fragment.hammerId && side.sideId != fragment.hammerId) continue;
            if (glm::dot(fragment.plane.normal, side.plane.normal) < kNormalAgreement) continue;
            auto loop = clipToSide(side, fragment.vertices);
            if (loop.size() < 3 || glm::length(newell(loop)) < 2 * kMinPieceArea) continue;
            // The BSP winds clockwise about the lit normal; Apex loops are counter-clockwise.
            if (glm::dot(newell(loop), side.plane.normal) < 0) std::reverse(loop.begin(), loop.end());
            BakeFace face = chartFace;
            face.vertices = std::move(loop);
            face.normal = side.plane.normal;
            face.solidId = side.solidId;
            face.sideId = side.sideId;
            result.faces.push_back(std::move(face));
            result.radiance.push_back(fragment.luxels);
            sides.insert({side.solidId, side.sideId});
            ++pieces;
        }
        if (!pieces) { ++result.stats.unowned; continue; }
        if (pieces > 1) ++result.stats.shared;
        ++result.stats.mapped;
    }
    result.stats.faces = sides.size();
    return result;
}

} // namespace apex::lighting
