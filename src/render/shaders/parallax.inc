#ifndef APEX_PARALLAX_INC
#define APEX_PARALLAX_INC
// Transcribes lighting::parallaxOcclusionUv (specular.hpp). The including shader defines the height
// lookup; slot and gradients pass through to it.
float parallaxHeight(vec2 uv, uint slot, vec2 duvdx, vec2 duvdy);

int parallaxLayers(float viewZ, float scale) {
    return int(ceil(mix(32.0, 8.0, clamp(viewZ, 0.0, 1.0)) * clamp(scale, 1.0, 4.0)));
}

vec2 parallaxOcclusionUv(vec2 uv, vec3 tangentView, float depth, float center, float scale,
                         uint slot, vec2 duvdx, vec2 duvdy) {
    vec2 shift = tangentView.xy / max(tangentView.z, 0.05) * depth;
    int layers = parallaxLayers(tangentView.z, scale);
    vec2 previous = uv + shift * (1.0 - center);
    float previousGap = parallaxHeight(previous, slot, duvdx, duvdy) - 1.0;
    if (previousGap >= 0.0) return previous;
    for (int i = 1; i <= layers; ++i) {
        float level = 1.0 - float(i) / float(layers);
        vec2 at = uv + shift * (level - center);
        float gap = parallaxHeight(at, slot, duvdx, duvdy) - level;
        if (gap >= 0.0) return mix(at, previous, gap / (gap - previousGap));
        previous = at;
        previousGap = gap;
    }
    return previous;
}
#endif
