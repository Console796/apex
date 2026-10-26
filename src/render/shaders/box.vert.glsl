#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.inc"
#include "preview.inc"
// Entity boxes, one instance per box, vertices generated from gl_VertexIndex. p0.x > 0.5 draws
// the 36-vertex solid (triangle list); otherwise the 24-vertex wire (line list).

layout(location = 0) in vec3 inMins;
layout(location = 1) in vec3 inMaxs;
layout(location = 2) in vec4 inColour;
layout(location = 3) in vec3 inOrigin;
layout(location = 4) in mat3 inBasis;

layout(location = 0) out vec4 vColour;

const int kQuads[24] = int[24](0, 2, 6, 4,  1, 3, 7, 5,  0, 1, 5, 4,  2, 3, 7, 6,  0, 1, 3, 2,  4, 5, 7, 6);
const vec3 kNormals[6] = vec3[6](vec3(-1, 0, 0), vec3(1, 0, 0), vec3(0, -1, 0), vec3(0, 1, 0), vec3(0, 0, -1), vec3(0, 0, 1));
const int kTri[6] = int[6](0, 1, 2, 0, 2, 3);

void main() {
    int v = gl_VertexIndex;
    int corner;
    float shade = 1.0;
    if (push.p0.x > 0.5) {
        int face = v / 6;
        corner = kQuads[face * 4 + kTri[v % 6]];
        shade = 0.6 + 0.4 * abs(dot(previewNormal(inBasis * kNormals[face]), normalize(vec3(0.3, 0.5, 0.8))));
    } else {
        int e = v / 2;
        int axis = e / 4;
        int r = e % 4;
        int a1 = (axis + 1) % 3;
        int a2 = (axis + 2) % 3;
        corner = ((r & 1) << a1) | (((r >> 1) & 1) << a2) | ((v & 1) << axis);
    }
    vec3 p = mix(inMins, inMaxs, vec3(bvec3((corner & 1) != 0, (corner & 2) != 0, (corner & 4) != 0)));
    gl_Position = push.viewProj * vec4(previewPosition(inOrigin + inBasis * p), 1.0);
    vColour = push.p0.y > 0.5 ? push.colour
                              : vec4(mix(inColour.rgb * shade, push.colour.rgb, push.colour.a), inColour.a);
}
