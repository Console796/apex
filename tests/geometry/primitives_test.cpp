#include "fixtures.hpp"
#include "geometry/primitives.hpp"

using namespace apex;
using namespace apex::geometry;

TEST_CASE("primitive generators validate across map corners and parameter counts") {
    for (double corner : {-65536.0, 0.0, 65536.0})
        for (int count : {3, 4, 8, 16, 64})
            for (auto type : {PrimitiveType::Wedge, PrimitiveType::Cylinder, PrimitiveType::Spike,
                              PrimitiveType::Arch, PrimitiveType::Sphere}) {
                CAPTURE(corner); CAPTURE(count); CAPTURE(static_cast<int>(type));
                PrimitiveBox box;
                box.origin = DVec3(corner);
                box.bounds = corner > 0 ? AABB{{-256, -192, -128}, {0, 0, 0}}
                                        : AABB{{0, 0, 0}, {256, 192, 128}};
                PrimitiveParameters p;
                p.type = type; p.sides = count; p.segments = count;
                auto output = primitive(box, p);
                REQUIRE(output);
                CHECK(output->size() == ((type == PrimitiveType::Arch || type == PrimitiveType::Sphere)
                                         ? static_cast<std::size_t>(count) : 1));
                for (const auto& poly : *output) {
                    CHECK(poly.validate().empty());
                    if (type == PrimitiveType::Cylinder) CHECK(poly.faces.size() == count + 2);
                    if (type == PrimitiveType::Spike) CHECK(poly.faces.size() == count + 1);
                    if (type == PrimitiveType::Wedge) CHECK(poly.faces.size() == 5);
                    if (type == PrimitiveType::Arch) CHECK(poly.faces.size() == 6);
                    for (auto v : poly.vertices) {
                        const auto local = v - box.origin;
                        for (int axis = 0; axis < 3; ++axis) {
                            CHECK(local[axis] >= box.bounds.min[axis] - 1e-6);
                            CHECK(local[axis] <= box.bounds.max[axis] + 1e-6);
                        }
                    }
                }
            }
}

TEST_CASE("primitive arch angles width and successive height follow its frame") {
    for (int count : {3, 8, 64, 100})
        for (double arc : {8.0, 90.0, 180.0, 360.0})
            for (double start : {0.0, 37.0, 270.0})
                for (double width : {4.0, 32.0})
                    for (double rise : {-2.0, 0.0, 4.0}) {
                        CAPTURE(count); CAPTURE(arc); CAPTURE(start); CAPTURE(width); CAPTURE(rise);
                        PrimitiveBox box;
                        box.origin = {65000, -65000, 65000};
                        box.axes = {DVec3{0, 1, 0}, DVec3{0, 0, 1}, DVec3{1, 0, 0}};
                        box.bounds = {{0, 0, 0}, {256, 192, 64}};
                        PrimitiveParameters p{PrimitiveType::Arch, count, arc, start, width, rise};
                        const auto output = primitive(box, p);
                        REQUIRE(output);
                        REQUIRE(output->size() == count);
                        for (int i = 0; i < count; ++i) {
                            const auto& poly = (*output)[i];
                            CHECK(poly.isValid());
                            for (const auto v : poly.vertices) {
                                const auto d = v - box.origin;
                                const DVec3 local{glm::dot(d, box.axes[0]), glm::dot(d, box.axes[1]), glm::dot(d, box.axes[2])};
                                CHECK(local.x >= -1e-6); CHECK(local.x <= 256 + 1e-6);
                                CHECK(local.y >= -1e-6); CHECK(local.y <= 192 + 1e-6);
                                CHECK(local.z >= rise * i - 1e-6); CHECK(local.z <= 64 + rise * i + 1e-6);
                            }
                            const auto bounds = poly.bounds();
                            CHECK(bounds.max.x <= box.origin.x + 64 + std::max(0.0, rise * i) + 1e-6); // Local z is world x.
                            const auto localBottom = glm::dot(bounds.min - box.origin, box.axes[2]);
                            CHECK(localBottom == doctest::Approx(rise * i));
                        }
                    }
}

TEST_CASE("primitive sphere bands share rings without overlapping interiors") {
    PrimitiveBox box;
    box.bounds = {{-64, -64, -64}, {64, 64, 64}};
    for (int segments : {3, 4, 8, 64}) {
        PrimitiveParameters p;
        p.type = PrimitiveType::Sphere; p.segments = segments;
        const auto output = primitive(box, p);
        REQUIRE(output);
        for (int i = 0; i < segments; ++i) {
            CHECK((*output)[i].faces.size() == segments + ((i == 0 || i == segments - 1) ? 1 : 2));
            if (i > 0) CHECK((*output)[i - 1].bounds().max.z == doctest::Approx((*output)[i].bounds().min.z));
        }
    }
}

TEST_CASE("primitive generators reject invalid parameters without partial shapes") {
    PrimitiveBox box;
    box.bounds = {{0, 0, 0}, {128, 128, 64}};
    PrimitiveParameters p;
    for (int count : {0, 2, 65}) { p.sides = count; CHECK_FALSE(primitive(box, p)); }
    p = {}; p.type = PrimitiveType::Sphere; p.segments = 2;
    CHECK_FALSE(primitive(box, p));
    p = {}; p.type = PrimitiveType::Arch; p.wallWidth = 64;
    CHECK_FALSE(primitive(box, p));
    p = {}; box.bounds.max.z = 0;
    CHECK_FALSE(primitive(box, p));
}
