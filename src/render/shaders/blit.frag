#version 450

/*
 * blit.frag — 8-bit palette lookup.
 *
 * binding 0: R8_UNORM texture carrying raw palette indices (0–255).
 *            Sampled as float [0,1]; round(x*255) gives the integer index.
 *
 * binding 1: UBO with 256 vec4 palette entries, std140.
 *            Loaded from the PCX palette or set dynamically for fades.
 *            Alpha channel is unused (always 1.0 from the C side).
 *
 * This is the GPU-side equivalent of the original NITSHELL.DLL palette
 * expansion that happened during Flip().
 */

layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D u_indexed;

layout(set = 0, binding = 1, std140) uniform Palette {
    vec4 entries[256];
} u_palette;

void main() {
    float raw = texture(u_indexed, v_uv).r;
    int   idx = int(round(raw * 255.0));
    out_color = u_palette.entries[idx];
}
