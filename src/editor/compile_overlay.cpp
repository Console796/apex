#include "editor/compile_overlay.hpp"

#include <cmath>

namespace apex::editor {
namespace {
Vec4 clusterColour(int cluster, bool stale) {
    if (stale) return Vec4(0.55f, 0.55f, 0.55f, 0.5f);
    // Golden-ratio hue steps keep neighbouring clusters apart.
    const float hue = std::fmod(static_cast<float>(cluster) * 0.61803399f, 1.0f) * 6.0f;
    const float x = 1.0f - std::fabs(std::fmod(hue, 2.0f) - 1.0f);
    const int sector = static_cast<int>(hue);
    constexpr float low = 0.25f;
    const float r[] = {1, x, low, low, x, 1}, g[] = {x, 1, 1, x, low, low}, b[] = {low, low, x, 1, 1, x};
    return Vec4(r[sector % 6], g[sector % 6], b[sector % 6], 0.9f);
}
}

CompileOverlay loadCompileOverlay(const std::filesystem::path& workDirectory, const std::string& name,
                                  std::uint64_t revision) {
    CompileOverlay overlay;
    overlay.revision = revision;
    overlay.workDirectory = workDirectory;
    if (auto leak = compile::readLeakFile(workDirectory, name)) overlay.leak = std::move(*leak);
    if (auto portals = compile::readPortalFile(workDirectory, name)) overlay.portals = std::move(*portals);
    return overlay;
}

ToolOverlay compileOverlayPrimitives(const CompileOverlay& overlay, bool stale, bool leak, bool portals) {
    ToolOverlay out;
    if (portals && overlay.showPortals) {
        for (const auto& portal : overlay.portals.portals) {
            if (overlay.cluster && portal.clusterA != *overlay.cluster && portal.clusterB != *overlay.cluster) continue;
            const auto colour = clusterColour(portal.clusterA, stale);
            for (std::size_t i = 0; i < portal.points.size(); ++i)
                out.worldSegments.push_back({portal.points[i], portal.points[(i + 1) % portal.points.size()], colour, 1.0f});
        }
    }
    if (leak && overlay.showLeak && !overlay.leak.empty()) {
        const Vec4 colour = stale ? Vec4(0.6f, 0.6f, 0.6f, 0.7f) : Vec4(1.0f, 0.1f, 0.1f, 1.0f);
        for (std::size_t i = 1; i < overlay.leak.size(); ++i)
            out.worldSegments.push_back({overlay.leak[i - 1], overlay.leak[i], colour, 3.0f});
        out.worldPoints.push_back({overlay.leak.front(), stale ? colour : Vec4(1.0f, 0.85f, 0.2f, 1.0f), 11.0f});
        out.labels.push_back({overlay.leak.front(), "Leak start", DVec2(8.0, -8.0), colour});
    }
    return out;
}

} // namespace apex::editor
