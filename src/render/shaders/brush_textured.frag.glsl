#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.inc"
#include "heap.inc"
#include "frame.inc"
#include "lighting.inc"
#include "shadows.inc"
#include "clusters.inc"
#include "material.inc"
#include "normal_map.inc"
#include "specular.inc"
#include "parallax.inc"

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUv;
layout(location = 2) flat in uint vMaterial;
layout(location = 3) in vec3 vPosition;
layout(location = 4) in vec2 vLightmapUv;
layout(location = 0) out vec4 outColor;

void main() {
    // The draw's texture slot, or the per-vertex material index when the draw passes 0xFFFFFFFF.
    uint slot = pushSlot() == 0xFFFFFFFFu ? vMaterial : pushSlot();
    // Derivatives must be evaluated before alpha-test discard and divergent material branches.
    vec3 dpdx = dFdx(vPosition), dpdy = dFdy(vPosition);
    vec2 duvdx = dFdx(vUv), duvdy = dFdy(vUv);
    float shade = 0.65 + 0.35 * abs(dot(normalize(vNormal), normalize(vec3(0.3, 0.5, 0.8))));
    vec4 texel = sampleSlot(slot, vUv);
    // p1.w (unused by the brush vertex shader): 0 opaque, 1 alpha test, 2 blend, 3 add (AlphaMode);
    // bit 2 outputs only the highlight tint, covering what the mesh covers.
    uint alphaMode = floatBitsToUint(push.p1.w) & 3u;
    bool tintOnly = (floatBitsToUint(push.p1.w) & 4u) != 0u;
    float alpha = alphaMode == 0u ? push.colour.a : push.colour.a * texel.a;
    if (alphaMode == 1u) {
        if (alpha < 0.5) discard;
        alpha = 1.0;
    }
    outColor = vec4(texel.rgb * push.colour.rgb * shade, alpha);
    // Additive layers are mostly tinted by $color2 (often to black, leaving only specular), so
    // Textured applies it too, back in the VMT's gamma space.
    if (alphaMode == 3u && (floatBitsToUint(push.p1.w) & 8u) == 0u) {
        Material material = materialRecord(frameBlock(), slot);
        if ((material.flags & MATERIAL_COLOUR2) != 0u) outColor.rgb *= pow(material.colour2.rgb, vec3(1.0 / 2.2));
    }
    if ((floatBitsToUint(push.p1.w) & 8u) != 0u) {
        FrameBlock frame = frameBlock();
        Material material = materialRecord(frame, slot);
        vec3 eyePosition = vPosition;
        if ((floatBitsToUint(push.p1.w) & 16u) != 0u)
            eyePosition += frame.skyTransform.xyz / frame.skyTransform.w;
        vec3 geometricNormal = normalize(vNormal);
        vec3 normal = geometricNormal;
        float bumpAlpha = 1.0;
        bool fullbright = (frame.flags & 1u) != 0u;
        bool localRefract = (material.flags & MATERIAL_LOCAL_REFRACT) != 0u;
        vec3 mapped = vec3(0.0, 0.0, 1.0);
        // PBR parallax moves every material read, so it is part of the surface's look and kept in fullbright.
        vec2 uv = vUv;
        if ((material.flags & MATERIAL_PARALLAX) != 0u) {
            vec3 t, b;
            tangentFrame(geometricNormal, dpdx, dpdy, duvdx, duvdy, t, b);
            vec3 eye = dot(eyePosition, eyePosition) > 1e-12 ? normalize(-eyePosition) : geometricNormal;
            uv = parallaxOcclusionUv(vUv, vec3(dot(eye, t), dot(eye, b), dot(eye, geometricNormal)),
                                     material.parallaxParams.x, material.parallaxParams.y,
                                     material.parallaxParams.z, material.bumpSlot, duvdx, duvdy);
            texel = textureGrad(sampler2D(textures[nonuniformEXT(material.baseSlot)], samplers[0]), uv, duvdx, duvdy);
        }
        // The light-cover offset is part of the surface's look, so it is kept in fullbright.
        if ((!fullbright || localRefract) && (material.flags & MATERIAL_HAS_BUMP) != 0u) {
            vec4 bump = textureGrad(sampler2D(textures[nonuniformEXT(material.bumpSlot)], samplers[0]),
                                    uv, duvdx, duvdy);
            bumpAlpha = bump.a;
            mapped = decodeNormal(bump.rgb, (material.flags & MATERIAL_SSBUMP) != 0u,
                                  (material.flags & MATERIAL_TWO_CHANNEL_NORMAL) != 0u);
            normal = perturbNormal(normal, dpdx, dpdy, duvdx, duvdy, mapped);
        }
        if (localRefract) {
            vec3 t, b;
            tangentFrame(geometricNormal, dpdx, dpdy, duvdx, duvdy, t, b);
            vec3 eye = dot(eyePosition, eyePosition) > 1e-12 ? normalize(-eyePosition) : geometricNormal;
            vec2 refractUv = localRefractUv(vUv, mapped, vec3(dot(eye, t), dot(eye, b), dot(eye, geometricNormal)),
                                            material.refractParams.y, material.refractParams.z,
                                            uvPerUnit(geometricNormal, dpdx, dpdy, duvdx, duvdy));
            texel = textureGrad(sampler2D(textures[nonuniformEXT(material.baseSlot)], samplers[0]), refractUv, duvdx, duvdy);
        }
        vec3 albedo = max(texel.rgb * push.colour.rgb, vec3(0.0));
        if ((material.flags & MATERIAL_LINEAR_BASE) == 0u) albedo = srgbToLinear(albedo);
        if ((material.flags & MATERIAL_COLOUR2) != 0u) albedo *= material.colour2.rgb;
        vec3 colour = albedo;
        bool pbr = (material.flags & MATERIAL_PBR) != 0u;
        vec3 mrao = vec3(0.0, 1.0, 1.0);
        if (pbr) {
            if (material.mraoSlot != NO_TEXTURE)
                mrao = textureGrad(sampler2D(textures[nonuniformEXT(material.mraoSlot)], samplers[0]),
                                   uv, duvdx, duvdy).rgb;
            mrao = clamp(mrao * material.mraoScale.xyz, vec3(0.0), vec3(1.0));
            // MRAO alpha is a blend-tint mask, reserved for step (f), never opacity/AO.
        }
        // Fragment positions and lights are camera-relative; camera is the origin.
        vec3 view = dot(eyePosition, eyePosition) > 1e-12 ? normalize(-eyePosition) : normal;
        vec3 diffuse = vec3(1.0); // Fullbright: unit irradiance.
        bool baked = (frame.flags & 4u) != 0u && (floatBitsToUint(push.p1.w) & 32u) != 0u;
        if (baked) {
            uint atlas = floatBitsToUint(frame.fill.z);
            diffuse = vec3(0.0); // Unmapped brush faces have black diffuse.
            if (atlas != NO_TEXTURE && all(greaterThanEqual(vLightmapUv, vec2(0.0))))
                diffuse = textureLod(sampler2D(textures[nonuniformEXT(atlas)], samplers[3]), vLightmapUv, 0.0).rgb * frame.fill.y;
        }
        bool toolFixed = (floatBitsToUint(push.p1.w) & 64u) != 0u;
        if (toolFixed) {
            diffuse = vec3(frame.fill.w); // Tool texture overlay: fixed brightness, no lighting.
            colour = albedo * diffuse;
        } else if (baked || !fullbright) {
            bool phong = (material.flags & MATERIAL_PHONG) != 0u;
            float exponent = material.phongParams.x;
            if (phong && material.phongExponentSlot != NO_TEXTURE)
                exponent = phongTextureExponent(textureGrad(
                    sampler2D(textures[nonuniformEXT(material.phongExponentSlot)], samplers[0]),
                    uv, duvdx, duvdy).r);
            float mask = (material.flags & MATERIAL_BASE_ALPHA_PHONG_MASK) != 0u ? texel.a : bumpAlpha;
            vec3 specular = vec3(0.0);
            if (!baked) diffuse = vec3(0.0);
            uint cell = clusterIndex(frame, vPosition);
            uint localCount = ClusterBuffer(frame.clusters).cells[cell].count;
            // Styles stay at full brightness. Sun and ambient are a global prefix (D14).
            for (uint entry = 0u; entry < frame.clusterDims.w + localCount; ++entry) {
                uint i = entry < frame.clusterDims.w ? entry :
                    ClusterBuffer(frame.clusters).cells[cell].indices[entry - frame.clusterDims.w];
                Light light = LightBuffer(frame.lights).lights[i];
                vec3 energy = irradiance(light, vPosition, normal, frame) *
                    shadowVisibility(frame, light, vPosition, geometricNormal);
                if (!baked && hasDirect(light)) diffuse += energy * (light.kindMode.x == 5u ? mrao.b : 1.0);
                if ((!pbr && !phong) || !hasSpecular(light)) continue;
                vec3 delta = light.kindMode.x == 3u ? -light.directionInner.xyz : light.originRadius.xyz - vPosition;
                if (dot(delta, delta) <= 1e-12) continue;
                vec3 toLight = normalize(delta);
                vec3 brdf = pbr ? ggxBrdf(normal, view, toLight, albedo, mrao.r, mrao.g) :
                    phongBrdf(normal, view, toLight, exponent, material.phongParams.y,
                              material.phongFresnel.xyz, material.phongTint.xyz, mask);
                specular += specularIrradiance(energy, dot(normal, toLight), brdf);
            }
            // Constant bounce stand-in, a touch brighter facing up; the AO in the display pass darkens it.
            if (!baked) diffuse += vec3(frame.fill.x * (0.8 + 0.2 * normal.z)) * mrao.b;
            colour = albedo * (1.0 - mrao.r) * diffuse + specular;
        }
        // Unlit: the cover shows its base texture through the tint; lighting still drives $envmaplightscale.
        if (localRefract) colour = albedo * max(material.refractTint.rgb, vec3(0.0)) *
            localRefractTransmission(mapped.z);
        // Unlit terms, kept in fullbright: environment reflection and self-illumination.
        if ((material.flags & MATERIAL_ENVMAP) != 0u && (frame.flags & 2u) == 0u) {
            uint cube = (material.flags & MATERIAL_ENVMAP_PROBE) != 0u ?
                nearestCubemap(frame, push.p0.a > 0.0 ? push.p0.xyz : vPosition) : material.envmapSlot;
            vec3 reflected = reflect(-view, normal);
            if ((material.flags & MATERIAL_ENVMAP_PROBE) != 0u && frame.cubemapCount > 0u) {
                uint index = nearestCubemapIndex(frame, push.p0.a > 0.0 ? push.p0.xyz : vPosition);
                reflected = correctedReflection(CubemapProbeBuffer(frame.cubemaps).probes[index], vPosition, reflected);
            }
            if (pbr) {
                float levels = float(textureQueryLevels(samplerCube(cubes[nonuniformEXT(cube)], samplers[0])));
                vec3 radiance = textureLod(samplerCube(cubes[nonuniformEXT(cube)], samplers[0]),
                                           reflected, mrao.g * (levels - 1.0)).rgb;
                colour += envmapContrastSaturation(radiance, material.envmapParams.x, material.envmapParams.y) * envBrdf(specularF0(albedo, mrao.r), mrao.g, dot(normal, view)) *
                          mrao.b * max(material.envmapTint.rgb, vec3(0.0)) *
                          envmapLightScale(diffuse, material.envmapTint.w);
            } else {
                bool hasMask = material.envmapMaskSlot != NO_TEXTURE;
                vec3 maskTexel = hasMask ? textureGrad(sampler2D(textures[nonuniformEXT(material.envmapMaskSlot)],
                                                                 samplers[0]), uv, duvdx, duvdy).rgb : vec3(1.0);
                // Base mip: these reflections are sharp, and the derivative LOD is not uniform here.
                vec3 radiance = textureLod(samplerCube(cubes[nonuniformEXT(cube)], samplers[0]), reflected, 0.0).rgb;
                colour += envmapContrastSaturation(radiance, material.envmapParams.x, material.envmapParams.y) *
                          max(material.envmapTint.rgb, vec3(0.0)) * envmapLightScale(diffuse, material.envmapTint.w) *
                          envmapMask(hasMask, maskTexel, (material.flags & MATERIAL_NORMAL_ALPHA_ENVMAP_MASK) != 0u,
                                     bumpAlpha, (material.flags & MATERIAL_BASE_ALPHA_ENVMAP_MASK) != 0u, texel.a);
            }
        }
        if ((material.flags & MATERIAL_SELFILLUM) != 0u) {
            bool hasMask = material.selfIllumMaskSlot != NO_TEXTURE;
            float maskRed = hasMask ? textureGrad(sampler2D(textures[nonuniformEXT(material.selfIllumMaskSlot)],
                                                            samplers[0]), uv, duvdx, duvdy).r : 1.0;
            colour += albedo * max(material.selfIllumTint.rgb, vec3(0.0)) * selfIllumMask(hasMask, maskRed, texel.a);
        }
        if (frame.fogParams.w > 0.0) {
            vec3 fogPosition = vPosition;
            if ((floatBitsToUint(push.p1.w) & 16u) != 0u)
                fogPosition = fogPosition * frame.skyTransform.w + frame.skyTransform.xyz;
            float distance = length(fogPosition);
            float fog = frame.fogParams.y > frame.fogParams.x ?
                clamp((distance - frame.fogParams.x) / (frame.fogParams.y - frame.fogParams.x), 0.0, 1.0) :
                step(frame.fogParams.x, distance);
            colour = mix(colour, frame.fogColour.rgb, fog * frame.fogParams.z);
        }
        outColor = vec4(colour, alpha);
    }
    if ((floatBitsToUint(push.p1.w) & 8u) == 0u)
        outColor.rgb = mix(outColor.rgb, push.p0.rgb, push.p0.a);
    if (tintOnly) outColor = vec4(push.p0.rgb, push.p0.a * alpha);
}

float parallaxHeight(vec2 uv, uint slot, vec2 duvdx, vec2 duvdy) {
    return textureGrad(sampler2D(textures[nonuniformEXT(slot)], samplers[0]), uv, duvdx, duvdy).a;
}
