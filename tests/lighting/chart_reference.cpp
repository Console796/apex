// Optional read-only BSP extent check; see lighting_0_7_2_charting.md for its limits.
#include "lighting/charting.hpp"

#include <bsppp/bsppp.h>

#include <cstring>
#include <iostream>
#include <stdexcept>

using namespace apex;

namespace {
template <typename V> DVec3 vec(const V& v) { return {v[0], v[1], v[2]}; }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: apex_chart_reference file.bsp ...\n";
        return 2;
    }
    std::size_t checked = 0, failures = 0;
    for (int arg = 1; arg < argc; ++arg) {
        bsppp::BSP bsp(argv[arg]);
        if (!bsp) { std::cerr << "cannot open " << argv[arg] << '\n'; return 2; }
        const auto faces = bsp.getLumpData<bsppp::BSPLump::FACES>();
        const auto planes = bsp.getLumpData<bsppp::BSPLump::PLANES>();
        const auto texinfo = bsp.getLumpData<bsppp::BSPLump::TEXINFO>();
        const auto vertices = bsp.getLumpData<bsppp::BSPLump::VERTEXES>();
        const auto edges = bsp.getLumpData<bsppp::BSPLump::EDGES>();
        const auto surfedges = bsp.getLumpData<bsppp::BSPLump::SURFEDGES>();
        const auto ids = bsp.getLumpData<bsppp::BSPLump::FACEIDS>();
        for (std::size_t i = 0; i < faces.size(); ++i) {
            const auto& f = faces[i];
            if (f.dispInfo != -1 || f.texInfo < 0 || f.numEdges < 3) continue;
            const auto& ti = texinfo.at(static_cast<std::size_t>(f.texInfo));
            if (ti.flags & (0x4 | 0x400)) continue; // sky / SURF_NOLIGHT
            geometry::Face face;
            face.lightmapScale = 1; // Already-scaled BSP texinfo, with its offsets.
            face.uv.uaxis = vec(ti.lightmapVector1);
            face.uv.vaxis = vec(ti.lightmapVector2);
            face.uv.shiftU = ti.lightmapVector1[3];
            face.uv.shiftV = ti.lightmapVector2[3];
            const auto& plane = planes.at(f.planeNum);
            face.plane = {vec(plane.normal), plane.dist};
            std::vector<DVec3> points;
            for (int e = 0; e < f.numEdges; ++e) {
                const auto se = surfedges.at(static_cast<std::size_t>(f.firstEdge + e)).surfEdge;
                const auto& edge = edges.at(static_cast<std::size_t>(std::abs(se)));
                face.vertices.push_back(points.size());
                points.push_back(vec(vertices.at(se >= 0 ? edge.v0 : edge.v1).position));
            }
            // FACEIDS is an optional uint16 per emitted face in the standard lump.
            std::uint16_t id = 0;
            if (ids && ids->size() == faces.size() * sizeof(id))
                std::memcpy(&id, ids->data() + i * sizeof(id), sizeof(id));
            ++checked;
            try {
                const auto result = lighting::chartFace(face, points);
                bool match = true;
                for (std::uint8_t a = 0; a < 2; ++a)
                    match &= result.chart.mins[static_cast<std::size_t>(a)] == f.lightmapTextureMinsInLuxels[a] &&
                             result.chart.extents[static_cast<std::size_t>(a)] == f.lightmapTextureSizeInLuxels[a];
                if (!match) {
                    ++failures;
                    std::cerr << argv[arg] << " face=" << i << " hammerfaceid=" << id
                              << " BSP mins=" << f.lightmapTextureMinsInLuxels[0] << ',' << f.lightmapTextureMinsInLuxels[1]
                              << " extents=" << f.lightmapTextureSizeInLuxels[0] << ',' << f.lightmapTextureSizeInLuxels[1]
                              << " chart mins=" << result.chart.mins[0] << ',' << result.chart.mins[1]
                              << " extents=" << result.chart.extents[0] << ',' << result.chart.extents[1] << '\n';
                }
            } catch (const std::invalid_argument& error) {
                ++failures;
                std::cerr << argv[arg] << " face=" << i << " hammerfaceid=" << id << ": " << error.what() << '\n';
            }
        }
    }
    std::cout << "checked=" << checked << " failures=" << failures << '\n';
    return failures || checked == 0 ? 1 : 0;
}
