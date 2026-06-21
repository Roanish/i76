#version 450

/*
 * mesh.frag — palettized texture lookup for the 3D world pass.
 *
 * binding 0: R8_UNORM texture carrying raw palette indices (the decoded .map
 *            texels, or a 1x1 flat index for untextured faces). Sampled NEAREST
 *            with REPEAT addressing, mirroring the software rasterizer.
 * binding 1: the shared 256-entry palette UBO (same one the blit uses).
 *
 * Unlit on purpose: the software textured path (raster_tri_tex) writes the raw
 * texel index with no shading, so the hardware pass matches it texel-for-texel
 * for a direct SW/HW comparison (TAB in the world viewer).
 */

layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D u_tex;

layout(set = 0, binding = 1, std140) uniform Palette {
    vec4 entries[256];
} u_palette;

void main() {
    float raw = texture(u_tex, v_uv).r;
    int   idx = int(round(raw * 255.0));
    out_color = vec4(u_palette.entries[idx].rgb, 1.0);
}
