#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.inc"
#include "heap.inc"
#include "frame.inc"
#include "lighting.inc"
#include "material.inc"
#include "normal_map.inc"
#include "specular.inc"

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUv;
layout(location = 2) flat in uint vMaterial;
layout(location = 3) in vec3 vPosition;
layout(location = 0) out vec4 outColor;

void main() {
    // The draw's texture slot, or the per-vertex material index when the draw passes 0xFFFFFFFF.
    uint slot = pushSlot() == 0xFFFFFFFFu ? vMaterial : pushSlot();
    // Derivatives must be evaluated before alpha-test discard and divergent material branches.
    vec3 dpdx = dFdx(vPosition), dpdy = dFdy(vPosition);
    vec2 duvdx = dFdx(vUv), duvdy = dFdy(vUv);
    float shade = 0.65 + 0.35 * abs(dot(normalize(vNormal), normalize(vec3(0.3, 0.5, 0.8))));
    vec4 texel = sampleSlot(slot, vUv);
    // p1.w (unused by the brush vertex shader): 0 opaque, 1 alpha test, 2 blend (AlphaMode);
    // bit 2 outputs only the highlight tint, covering what the mesh covers.
    uint alphaMode = floatBitsToUint(push.p1.w) & 3u;
    bool tintOnly = (floatBitsToUint(push.p1.w) & 4u) != 0u;
    float alpha = alphaMode == 0u ? push.colour.a : push.colour.a * texel.a;
    if (alphaMode == 1u) {
        if (alpha < 0.5) discard;
        alpha = 1.0;
    }
    outColor = vec4(texel.rgb * push.colour.rgb * shade, alpha);
    if ((floatBitsToUint(push.p1.w) & 8u) != 0u) {
        FrameBlock frame = frameBlock();
        Material material = materialRecord(frame, slot);
        vec3 geometricNormal = normalize(vNormal);
        vec3 normal = geometricNormal;
        float bumpAlpha = 1.0;
        if ((frame.flags & 1u) == 0u && (material.flags & MATERIAL_HAS_BUMP) != 0u) {
            vec4 bump = textureGrad(sampler2D(textures[nonuniformEXT(material.bumpSlot)], samplers[0]),
                                    vUv, duvdx, duvdy);
            bumpAlpha = bump.a;
            vec3 mapped = decodeNormal(bump.rgb, (material.flags & MATERIAL_SSBUMP) != 0u,
                                       (material.flags & MATERIAL_TWO_CHANNEL_NORMAL) != 0u);
            normal = perturbNormal(normal, dpdx, dpdy, duvdx, duvdy, mapped);
        }
        vec3 albedo = max(texel.rgb * push.colour.rgb, vec3(0.0));
        if ((material.flags & MATERIAL_LINEAR_BASE) == 0u) albedo = pow(albedo, vec3(2.2));
        vec3 colour = albedo;
        bool fullbright = (frame.flags & 1u) != 0u;
        bool pbr = (material.flags & MATERIAL_PBR) != 0u;
        vec3 mrao = vec3(0.0, 1.0, 1.0);
        if (pbr) {
            if (material.mraoSlot != NO_TEXTURE)
                mrao = textureGrad(sampler2D(textures[nonuniformEXT(material.mraoSlot)], samplers[0]),
                                   vUv, duvdx, duvdy).rgb;
            mrao = clamp(mrao * material.mraoScale.xyz, vec3(0.0), vec3(1.0));
            // MRAO alpha is a blend-tint mask, reserved for step (f), never opacity/AO.
        }
        // Fragment positions and lights are camera-relative; camera is the origin.
        vec3 view = dot(vPosition, vPosition) > 1e-12 ? normalize(-vPosition) : normal;
        vec3 diffuse = vec3(1.0); // Fullbright: unit irradiance.
        if (!fullbright) {
            bool phong = (material.flags & MATERIAL_PHONG) != 0u;
            float exponent = material.phongParams.x;
            if (phong && material.phongExponentSlot != NO_TEXTURE)
                exponent = phongTextureExponent(textureGrad(
                    sampler2D(textures[nonuniformEXT(material.phongExponentSlot)], samplers[0]),
                    vUv, duvdx, duvdy).r);
            float mask = (material.flags & MATERIAL_BASE_ALPHA_PHONG_MASK) != 0u ? texel.a : bumpAlpha;
            vec3 specular = vec3(0.0);
            diffuse = vec3(0.0);
            // Styles stay at full brightness. 0.6.6 hook: split direct/specular light_rt modes here.
            for (uint i = 0u; i < frame.lightCount; ++i) {
                Light light = LightBuffer(frame.lights).lights[i];
                vec3 energy = irradiance(light, vPosition, normal, frame);
                diffuse += energy * (light.kindMode.x == 5u ? mrao.b : 1.0);
                if ((!pbr && !phong) || light.kindMode.x == 5u) continue;
                vec3 delta = light.kindMode.x == 3u ? -light.directionInner.xyz : light.originRadius.xyz - vPosition;
                if (dot(delta, delta) <= 1e-12) continue;
                vec3 toLight = normalize(delta);
                vec3 brdf = pbr ? ggxBrdf(normal, view, toLight, albedo, mrao.r, mrao.g) :
                    phongBrdf(normal, view, toLight, exponent, material.phongParams.y,
                              material.phongFresnel.xyz, material.phongTint.xyz, mask);
                specular += specularIrradiance(energy, dot(normal, toLight), brdf);
            }
            colour = albedo * (1.0 - mrao.r) * diffuse + specular;
        }
        // Unlit terms, kept in fullbright: environment reflection and self-illumination.
        if ((material.flags & MATERIAL_ENVMAP) != 0u) {
            uint cube = (material.flags & MATERIAL_ENVMAP_PROBE) != 0u ?
                nearestCubemap(frame, vPosition) : material.envmapSlot;
            vec3 reflected = reflect(-view, normal);
            if (pbr) {
                float levels = float(textureQueryLevels(samplerCube(cubes[nonuniformEXT(cube)], samplers[0])));
                vec3 radiance = textureLod(samplerCube(cubes[nonuniformEXT(cube)], samplers[0]),
                                           reflected, mrao.g * (levels - 1.0)).rgb;
                colour += radiance * envBrdf(specularF0(albedo, mrao.r), mrao.g, dot(normal, view)) *
                          mrao.b * max(material.envmapTint.rgb, vec3(0.0)) *
                          envmapLightScale(diffuse, material.envmapTint.w);
            } else {
                bool hasMask = material.envmapMaskSlot != NO_TEXTURE;
                vec3 maskTexel = hasMask ? textureGrad(sampler2D(textures[nonuniformEXT(material.envmapMaskSlot)],
                                                                 samplers[0]), vUv, duvdx, duvdy).rgb : vec3(1.0);
                // Base mip: these reflections are sharp, and the derivative LOD is not uniform here.
                colour += textureLod(samplerCube(cubes[nonuniformEXT(cube)], samplers[0]), reflected, 0.0).rgb *
                          max(material.envmapTint.rgb, vec3(0.0)) *
                          envmapMask(hasMask, maskTexel, (material.flags & MATERIAL_NORMAL_ALPHA_ENVMAP_MASK) != 0u,
                                     bumpAlpha, (material.flags & MATERIAL_BASE_ALPHA_ENVMAP_MASK) != 0u, texel.a);
            }
        }
        if ((material.flags & MATERIAL_SELFILLUM) != 0u) {
            bool hasMask = material.selfIllumMaskSlot != NO_TEXTURE;
            float maskRed = hasMask ? textureGrad(sampler2D(textures[nonuniformEXT(material.selfIllumMaskSlot)],
                                                            samplers[0]), vUv, duvdx, duvdy).r : 1.0;
            colour += albedo * max(material.selfIllumTint.rgb, vec3(0.0)) * selfIllumMask(hasMask, maskRed, texel.a);
        }
        outColor = vec4(colour, alpha);
    }
    outColor.rgb = mix(outColor.rgb, push.p0.rgb, push.p0.a);
    if (tintOnly) outColor = vec4(push.p0.rgb, push.p0.a * alpha);
}
