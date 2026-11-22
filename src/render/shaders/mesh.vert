#version 450

/*
 * mesh.vert — 3D world geometry pass.
 *
 * Per-vertex: object-space position, texture UV and OUTWARD normal (built
 * CPU-side, see worldview.c). Push constants: full model-view-projection and
 * the model matrix (its 3x3 rotates the normal into world space for the
 * global light, see mesh.frag).
 */

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec3 in_normal;

layout(push_constant) uniform Push {
    mat4 mvp;
    mat4 model;
} pc;

layout(location = 0) out vec2  v_uv;
layout(location = 1) out vec3  v_normal;   /* world space */
layout(location = 2) out float v_depth;    /* view-space distance (clip w) */

void main() {
    gl_Position = pc.mvp * vec4(in_pos, 1.0);
    v_uv     = in_uv;
    v_normal = mat3(pc.model) * in_normal;
    v_depth  = gl_Position.w;
}
