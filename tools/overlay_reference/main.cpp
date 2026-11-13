// Dumps the OVERLAYS lump of a compiled P2CE BSP for the D10 oracle (tools/overlay_oracle.py).
// Not shipped. Lump 45, version 3 (layout in docs/plans/probe-results/overlays_0_8_6_p0.md):
//
//   int numOverlays; int numFaceRefs; int faceRefs[numFaceRefs];
//   numOverlays x 108 bytes: int id, texinfo, unknown0, firstFaceRef, faceCount;
//   float U[2], V[2], uvPoints[4][2], basisU[3], origin[3], normal[3]; int unknown1.
//
// For each overlay it prints the raw fields, then each listed BSP face as its polygon (the face's
// vertices in BSP winding order) and its plane normal. Nothing is interpreted beyond that.
#include <bsppp/bsppp.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

namespace {
template <typename T> T read(const std::vector<std::byte>& data, std::size_t offset) {
    T value;
    std::memcpy(&value, data.data() + offset, sizeof(T));
    return value;
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: apex_overlay_reference file.bsp\n";
        return 2;
    }
    bsppp::BSP bsp(argv[1]);
    if (!bsp) { std::cerr << "cannot open " << argv[1] << '\n'; return 2; }
    const auto faces = bsp.getLumpData<bsppp::BSPLump::FACES>();
    const auto planes = bsp.getLumpData<bsppp::BSPLump::PLANES>();
    const auto vertices = bsp.getLumpData<bsppp::BSPLump::VERTEXES>();
    const auto edges = bsp.getLumpData<bsppp::BSPLump::EDGES>();
    const auto surfedges = bsp.getLumpData<bsppp::BSPLump::SURFEDGES>();
    const auto lump = bsp.getLumpData(bsppp::BSPLump::OVERLAYS);
    std::printf("bspversion %u overlays_version %u\n", bsp.getVersion(), bsp.getLumpVersion(bsppp::BSPLump::OVERLAYS));
    if (!lump || lump->size() < 8) { std::printf("count 0\n"); return 0; }
    const auto count = read<std::int32_t>(*lump, 0);
    const auto refs = read<std::int32_t>(*lump, 4);
    const std::size_t table = 8 + static_cast<std::size_t>(refs) * 4;
    constexpr std::size_t kStride = 108;
    if (count < 0 || refs < 0 || table + static_cast<std::size_t>(count) * kStride != lump->size()) {
        std::cerr << "unexpected OVERLAYS layout: " << count << " overlays, " << refs << " refs, " << lump->size()
                  << " bytes\n";
        return 1;
    }
    std::printf("count %d\n", count);
    for (std::int32_t n = 0; n < count; ++n) {
        const std::size_t at = table + static_cast<std::size_t>(n) * kStride;
        const auto f = [&](std::size_t word) { return read<float>(*lump, at + word * 4); };
        const auto i = [&](std::size_t word) { return read<std::int32_t>(*lump, at + word * 4); };
        // id texinfo unknown0 unknown1 | U0 U1 V0 V1 | uv0..uv3 (x y) | basisU | origin | normal
        std::printf("overlay %d %d %d %d range %.9g %.9g %.9g %.9g uv", i(0), i(1), i(2), i(26), f(5), f(6), f(7), f(8));
        for (std::size_t k = 9; k < 17; ++k) std::printf(" %.9g", f(k));
        std::printf(" basisu %.9g %.9g %.9g origin %.9g %.9g %.9g normal %.9g %.9g %.9g\n", f(17), f(18), f(19), f(20),
                    f(21), f(22), f(23), f(24), f(25));
        const auto first = i(3), faceCount = i(4);
        for (std::int32_t r = 0; r < faceCount; ++r) {
            const auto ref = read<std::int32_t>(*lump, 8 + static_cast<std::size_t>(first + r) * 4);
            const auto& face = faces.at(static_cast<std::size_t>(ref));
            const auto& plane = planes.at(face.planeNum);
            const float sign = face.side ? -1.0f : 1.0f;
            std::printf("face %d normal %.9g %.9g %.9g points", ref, sign * plane.normal[0], sign * plane.normal[1],
                        sign * plane.normal[2]);
            for (int e = 0; e < face.numEdges; ++e) {
                const auto se = surfedges.at(static_cast<std::size_t>(face.firstEdge + e)).surfEdge;
                const auto& edge = edges.at(static_cast<std::size_t>(se < 0 ? -se : se));
                const auto& p = vertices.at(se >= 0 ? edge.v0 : edge.v1).position;
                std::printf(" %.9g %.9g %.9g", p[0], p[1], p[2]);
            }
            std::printf("\n");
        }
    }
    return 0;
}
