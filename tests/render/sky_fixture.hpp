#pragma once

#include "lighting/sky_visibility.hpp"

namespace apex::render::testing {

// Two box rooms: a square sky hole in the first ceiling, a sealed annex at +X. Faces point into air.
inline std::vector<lighting::SkyTriangle> skyRoomTriangles() {
    std::vector<lighting::SkyTriangle> out;
    const auto quad = [&](DVec3 c, DVec3 u, DVec3 v, bool sky = false) {
        out.push_back({c - u - v, c + u - v, c + u + v, sky});
        out.push_back({c - u - v, c + u + v, c - u + v, sky});
    };
    for (double x : {0.0, 1024.0}) {
        quad({x - 256, 0, 256}, {0, 256, 0}, {0, 0, 256});
        quad({x + 256, 0, 256}, {0, -256, 0}, {0, 0, 256});
        quad({x, -256, 256}, {-256, 0, 0}, {0, 0, 256});
        quad({x, 256, 256}, {256, 0, 0}, {0, 0, 256});
        quad({x, 0, 0}, {256, 0, 0}, {0, 256, 0});
        if (x != 0) quad({x, 0, 512}, {-256, 0, 0}, {0, 256, 0});
    }
    quad({-192, 0, 512}, {-64, 0, 0}, {0, 256, 0});
    quad({192, 0, 512}, {-64, 0, 0}, {0, 256, 0});
    quad({0, -192, 512}, {-128, 0, 0}, {0, 64, 0});
    quad({0, 192, 512}, {-128, 0, 0}, {0, 64, 0});
    quad({0, 0, 512}, {-128, 0, 0}, {0, 128, 0}, true);
    // A solid block in the gap gives both buried and mixed neighbourhoods for sampling parity.
    quad({256, 0, 256}, {0, -256, 0}, {0, 0, 256});
    quad({768, 0, 256}, {0, 256, 0}, {0, 0, 256});
    quad({512, -256, 256}, {256, 0, 0}, {0, 0, 256});
    quad({512, 256, 256}, {-256, 0, 0}, {0, 0, 256});
    quad({512, 0, 0}, {-256, 0, 0}, {0, 256, 0});
    quad({512, 0, 512}, {256, 0, 0}, {0, 256, 0});
    return out;
}

inline lighting::SkyVisibilityGrid skyRoomGrid() {
    return lighting::buildSkyVisibility(skyRoomTriangles(), {{-256, -256, 0}, {1280, 256, 512}});
}

} // namespace apex::render::testing
