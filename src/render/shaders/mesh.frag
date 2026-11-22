#version 450

/*
 * mesh.frag — palettized texture lookup + global directional lighting.
 *
 * binding 0: R8_UNORM texture carrying raw palette indices (the decoded .map
 *            texels, or a 1x1 flat index for untextured faces). Sampled NEAREST
 *            with REPEAT addressing, mirroring the software rasterizer.
 * binding 1: the shared UBO: 256-entry palette, then the global lighting.
 *
 * Lighting follows the original's model per pixel (docs/REVERSING.md "Time of
 * day + global lighting"): a scalar intensity
 *     I = min(1, intensity * max(0, N.L) * haze + ambient)
 * is mapped to a shade row exactly as the span stage does, row = (1-I)*31.5,
 * and the albedo is scaled by the brightness measured from the mission's .lum
 * for that row (interpolated: smooth instead of 32 bands). Non-linear by data:
 * ~1.4x in full light, ~1.0x near I=0.78, ~0.21x unlit. Default curve = 1.
 */

layout(location = 0) in  vec2  v_uv;
layout(location = 1) in  vec3  v_normal;
layout(location = 2) in  float v_depth;
layout(location = 0) out vec4  out_color;

layout(set = 0, binding = 0) uniform sampler2D u_tex;

layout(set = 0, binding = 1, std140) uniform Palette {
    vec4 entries[256];
    vec4 light;       /* xyz: unit vector toward the light (world), w: intensity */
    vec4 params;      /* x ambient, y haze base, z haze slope, w haze min        */
    vec4 curve[32];   /* rgb brightness multiplier per .lum row                  */
} u;

void main() {
    float raw = texture(u_tex, v_uv).r;
    int   idx = int(round(raw * 255.0));
    vec3  albedo = u.entries[idx].rgb;

    float nl   = length(v_normal);
    vec3  n    = nl > 1e-6 ? v_normal / nl : vec3(0.0, 1.0, 0.0);
    float ndl  = max(dot(n, u.light.xyz), 0.0);
    float haze = clamp(u.params.y + u.params.z * v_depth, u.params.w, 1.0);
    float I    = min(1.0, u.light.w * ndl * haze + u.params.x);

    float row  = clamp((1.0 - I) * 31.5, 0.0, 31.0);
    int   r0   = int(floor(row));
    int   r1   = min(r0 + 1, 31);
    vec3  k    = mix(u.curve[r0].rgb, u.curve[r1].rgb, row - float(r0));

    out_color = vec4(albedo * k, 1.0);
}
