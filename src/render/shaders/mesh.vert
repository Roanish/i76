#version 450

/*
 * mesh.vert — 3D world geometry pass.
 *
 * Per-vertex: object-space position + texture UV (built CPU-side, see
 * worldview.c). The push constant carries the full model-view-projection
 * (column-major mat4, 64 bytes). Shading is deliberately absent — the fragment
 * shader samples the palettized texture directly, matching the software
 * rasterizer's unlit textured path (softraster.c) so SW and HW agree.
 */

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec2 in_uv;

layout(push_constant) uniform Push {
    mat4 mvp;
} pc;

layout(location = 0) out vec2 v_uv;

void main() {
    gl_Position = pc.mvp * vec4(in_pos, 1.0);
    v_uv = in_uv;
}
