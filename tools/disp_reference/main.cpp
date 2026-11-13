// Dumps the displacements of a compiled BSP for the D2 oracle (tools/disp_oracle.py). Not shipped.
//
// For each DISPINFO entry: power, startPosition, the base face's corners in BSP winding order,
// and every DISP_VERTS entry (vector * distance, alpha). It also prints each vertex as the engine
// rebuilds it: corners rotated so the first is the one nearest startPosition, row i along
// corner0->corner1, column j along corner0->corner3 (bilinear), plus vector * distance, and the
// DISP_TRIS tags. The rows are raw BSP data apart from that one stated rule.
#include <bsppp/bsppp.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

namespace {

struct V3 { double x = 0, y = 0, z = 0; };
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double length(V3 a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }

template <typename T> T read(const std::vector<std::byte>& data, std::size_t offset) {
    T value;
    std::memcpy(&value, data.data() + offset, sizeof(T));
    return value;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: apex_disp_reference file.bsp\n";
        return 2;
    }
    bsppp::BSP bsp(argv[1]);
    if (!bsp) { std::cerr << "cannot open " << argv[1] << '\n'; return 2; }
    const auto faces = bsp.getLumpData<bsppp::BSPLump::FACES>();
    const auto vertices = bsp.getLumpData<bsppp::BSPLump::VERTEXES>();
    const auto edges = bsp.getLumpData<bsppp::BSPLump::EDGES>();
    const auto surfedges = bsp.getLumpData<bsppp::BSPLump::SURFEDGES>();
    const auto info = bsp.getLumpData(bsppp::BSPLump::DISPINFO);
    const auto verts = bsp.getLumpData(bsppp::BSPLump::DISP_VERTS);
    const auto tris = bsp.getLumpData(bsppp::BSPLump::DISP_TRIS);
    if (!info || !verts || !tris) { std::cerr << "missing displacement lumps\n"; return 2; }
    std::size_t count = 0;
    for (const auto& face : faces) if (face.dispInfo >= 0) ++count;
    if (count == 0 || info->size() % count) { std::cerr << "no displacements, or odd DISPINFO size\n"; return 2; }
    const std::size_t stride = info->size() / count;
    std::printf("bspversion %u dispinfo_stride %zu count %zu\n", bsp.getVersion(), stride, count);
    for (const auto& face : faces) {
        if (face.dispInfo < 0) continue;
        const std::size_t base = static_cast<std::size_t>(face.dispInfo) * stride;
        // ddispinfo_t prefix: startPosition, DispVertStart, DispTriStart, power.
        const V3 start{read<float>(*info, base), read<float>(*info, base + 4), read<float>(*info, base + 8)};
        const auto vertStart = read<std::int32_t>(*info, base + 12);
        const auto triStart = read<std::int32_t>(*info, base + 16);
        const auto power = read<std::int32_t>(*info, base + 20);
        const int size = (1 << power) + 1;
        std::printf("disp %d power %d start %.9g %.9g %.9g\n", face.dispInfo, power, start.x, start.y, start.z);
        std::vector<V3> corners;
        for (int e = 0; e < face.numEdges; ++e) {
            const auto se = surfedges.at(static_cast<std::size_t>(face.firstEdge + e)).surfEdge;
            const auto& edge = edges.at(static_cast<std::size_t>(std::abs(se)));
            const auto& p = vertices.at(se >= 0 ? edge.v0 : edge.v1).position;
            corners.push_back({p[0], p[1], p[2]});
            std::printf("corner %.9g %.9g %.9g\n", corners.back().x, corners.back().y, corners.back().z);
        }
        if (corners.size() != 4) { std::cerr << "non-quad displacement face\n"; return 1; }
        std::size_t first = 0;
        for (std::size_t i = 1; i < 4; ++i)
            if (length(corners[i] - start) < length(corners[first] - start)) first = i;
        std::array<V3, 4> c;
        for (std::size_t i = 0; i < 4; ++i) c[i] = corners[(first + i) % 4];
        std::printf("startcorner %zu\n", first);
        for (int i = 0; i < size; ++i) {
            const double t = double(i) / (size - 1);
            const V3 left = c[0] + (c[1] - c[0]) * t, right = c[3] + (c[2] - c[3]) * t;
            for (int j = 0; j < size; ++j) {
                const std::size_t index = static_cast<std::size_t>(i * size + j);
                const std::size_t at = (static_cast<std::size_t>(vertStart) + index) * 20;
                const V3 vector{read<float>(*verts, at), read<float>(*verts, at + 4), read<float>(*verts, at + 8)};
                const double dist = read<float>(*verts, at + 12), alpha = read<float>(*verts, at + 16);
                const V3 flat = left + (right - left) * (double(j) / (size - 1));
                const V3 pos = flat + vector * dist;
                // vert index, vector xyz, distance, alpha, rebuilt position xyz.
                std::printf("vert %zu %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g\n", index, vector.x, vector.y,
                            vector.z, dist, alpha, pos.x, pos.y, pos.z);
            }
        }
        const int triangles = 2 * (size - 1) * (size - 1);
        std::printf("tris");
        for (int t = 0; t < triangles; ++t)
            std::printf(" %u", unsigned(read<std::uint16_t>(*tris, (static_cast<std::size_t>(triStart) + t) * 2)));
        std::printf("\n");
    }
    return 0;
}
