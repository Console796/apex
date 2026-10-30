#include "lighting/specular.hpp"
#include "lighting/light.hpp"

#include <doctest/doctest.h>

using namespace apex;
using namespace apex::lighting;

TEST_CASE("brdf: GGX F0 endpoints roughness peak and horizons") {
    const DVec3 n(0, 0, 1), albedo(0.2, 0.5, 0.8);
    CHECK(specularF0(albedo, 0) == DVec3(0.04));
    CHECK(specularF0(albedo, 1) == albedo);
    CHECK(schlickFresnel(albedo, 1) == albedo);
    CHECK(schlickFresnel(albedo, 0) == DVec3(1));
    double peak = 1e9;
    for (const double roughness : {0.04, 0.2, 0.5, 1.0}) {
        const auto value = ggxBrdf(n, n, n, albedo, 1, roughness);
        CHECK(value.r > 0);
        CHECK(value.r < peak);
        peak = value.r;
        CHECK(ggxBrdf(n, n, {1, 0, 0}, albedo, 1, roughness) == DVec3(0));
        CHECK(ggxBrdf(n, n, -n, albedo, 1, roughness) == DVec3(0));
        CHECK(ggxBrdf(n, -n, n, albedo, 1, roughness) == DVec3(0));
    }
    CHECK(specularIrradiance(DVec3(1), 0, DVec3(1)) == DVec3(0));
    CHECK(specularIrradiance(DVec3(1), -1, DVec3(1)) == DVec3(0));
}

TEST_CASE("brdf: GGX hemispherical reflected energy stays bounded") {
    // Numerical rendering-equation integral, independent of goldens and the GLSL transcription.
    // Uniform solid-angle quadrature; roughness >= .2 resolves the narrowest lobe here.
    constexpr int rings = 256, sectors = 256;
    const DVec3 n(0, 0, 1);
    for (const double roughness : {0.2, 0.5, 1.0})
        for (const double metalness : {0.0, 1.0})
            for (const double nv : {0.2, 1.0}) {
                const DVec3 view(std::sqrt(1 - nv * nv), 0, nv);
                DVec3 energy(0);
                for (int j = 0; j < rings; ++j) {
                    const double z = (j + 0.5) / rings, radius = std::sqrt(1 - z * z);
                    for (int i = 0; i < sectors; ++i) {
                        const double phi = (i + 0.5) * 2 * std::numbers::pi / sectors;
                        const DVec3 l(radius * std::cos(phi), radius * std::sin(phi), z);
                        energy += ggxBrdf(n, view, l, DVec3(0.8), metalness, roughness) * z;
                    }
                }
                energy *= 2 * std::numbers::pi / (rings * sectors);
                CAPTURE(roughness);
                CAPTURE(metalness);
                CAPTURE(nv);
                CAPTURE(energy.r);
                CHECK(energy.r > 0);
                CHECK(energy.r <= 1.0);
            }
}

TEST_CASE("brdf: phong Fresnel curve exponent texture and mask endpoints") {
    const DVec3 ranges(0.1, 0.3, 0.9), n(0, 0, 1);
    CHECK(phongFresnel(1, ranges) == doctest::Approx(0.1));
    CHECK(phongFresnel(0.5, ranges) == doctest::Approx(0.3));
    CHECK(phongFresnel(0, ranges) == doctest::Approx(0.9));
    CHECK(phongFresnel(0.75, ranges) == doctest::Approx(0.2));
    CHECK(phongFresnel(0.25, ranges) == doctest::Approx(0.6));
    CHECK(phongTextureExponent(0) == 1);
    CHECK(phongTextureExponent(1) == 150);
    CHECK(phongBrdf(n, n, n, 32, 2, ranges, DVec3(1), 0) == DVec3(0));
    CHECK(phongBrdf(n, n, n, 32, 2, ranges, DVec3(1), 1).r == doctest::Approx(0.2));
    CHECK(phongBrdf(n, n, -n, 32, 2, ranges, DVec3(1), 1) == DVec3(0));
}

TEST_CASE("specular: measured diffuse response recovers the same incoming light scale") {
    const DVec3 n(0, 0, 1), view(0, 0, 1);
    lighting::Light light;
    light.kind = LightKind::Spot;
    light.origin = {30, 0, 40};
    light.direction = {0, 0, -1};
    light.intensity = {2, 3, 4};
    light.constant = 1;
    light.linear = 0.02;
    light.quadratic = 0.001;
    light.stopdot = 0.9;
    light.stopdot2 = 0.7;
    light.exponent = 2;
    const auto l = glm::normalize(light.origin);
    const double nl = glm::dot(n, l);
    const auto brdf = ggxBrdf(n, view, l, DVec3(0.6), 1, 0.4);
    const auto result = specularIrradiance(irradiance(light, DVec3(0), n), nl, brdf);
    // Distance=50; spot axis cosine=.8; cone ramp=(.8-.7)/(.9-.7)=.5, squared.
    const auto incoming = light.intensity / (1.0 + 0.02 * 50 + 0.001 * 50 * 50) * 0.8 * 0.25;
    CHECK(glm::length(result - incoming * brdf * nl) < 1e-12);
}

TEST_CASE("brdf: envmap BRDF fit endpoints mask selection and light scale") {
    // Karis 2014 analytic fit: smooth mirrors keep F0 at normal incidence and reach 1 at grazing.
    CHECK(envBrdf(DVec3(1), 0, 1).r == doctest::Approx(1).epsilon(0.002));
    CHECK(envBrdf(DVec3(0.04), 0, 1).r == doctest::Approx(0.04 * 0.99415 + 0.00585).epsilon(1e-3));
    CHECK(envBrdf(DVec3(0.04), 0, 0).r == doctest::Approx(1.04 * 1.0425 - 0.04 - 0.04 * (1.04 * 1.0425 - 1.04)).epsilon(1e-9));
    CHECK(envBrdf(DVec3(0.04), 1, 0).r < envBrdf(DVec3(0.04), 0, 0).r); // Roughness damps grazing gain.
    CHECK(envBrdf(DVec3(1), 1, 1).r < envBrdf(DVec3(1), 0, 1).r);
    CHECK(envBrdf(DVec3(0.5), 2, -1) == envBrdf(DVec3(0.5), 1, 0)); // Inputs clamp to [0,1].
    for (const double roughness : {0.0, 0.25, 0.5, 1.0})
        for (const double nv : {0.3, 0.6, 1.0}) { // At grazing the fit lets F0 slightly lower the result.
            const auto metal = envBrdf(DVec3(1), roughness, nv), dielectric = envBrdf(DVec3(0.04), roughness, nv);
            CHECK(metal.r > dielectric.r);
            CHECK(dielectric.r > 0);
            CHECK(metal.r <= 1.05);
        }
    CHECK(luminance(DVec3(1)) == doctest::Approx(1));
    CHECK(envmapLightScale(DVec3(0.25), 0) == 1);
    CHECK(envmapLightScale(DVec3(0.25), 1) == doctest::Approx(0.25));
    CHECK(envmapLightScale(DVec3(0.25), 0.5) == doctest::Approx(0.625));
    const DVec3 texel(0.2, 0.4, 0.6);
    CHECK(envmapMask(true, texel, true, 0.3, true, 0.7) == texel);
    CHECK(envmapMask(false, texel, true, 0.3, true, 0.7) == DVec3(0.3));
    CHECK(envmapMask(false, texel, false, 0.3, true, 0.7) == DVec3(0.7));
    CHECK(envmapMask(false, texel, false, 0.3, false, 0.7) == DVec3(1));
    CHECK(selfIllumMask(true, 0.2, 0.9) == 0.2);
    CHECK(selfIllumMask(false, 0.2, 0.9) == 0.9);
}
