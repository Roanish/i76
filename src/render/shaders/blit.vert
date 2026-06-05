#version 450

/*
 * blit.vert — fullscreen triangle, no vertex buffer needed.
 *
 * Three vertices cover the entire clip space:
 *   V0: (-1,-1)  UV (0,0)  top-left
 *   V1: ( 3,-1)  UV (2,0)  off-screen right
 *   V2: (-1, 3)  UV (0,2)  off-screen bottom
 *
 * The clipped visible region is exactly (0,0)→(1,1) in UV space,
 * which maps 1:1 to the index texture.
 *
 * Vulkan NDC: +Y is down, matching the original's top-left pixel origin.
 */

layout(location = 0) out vec2 v_uv;

void main() {
    vec2 uv = vec2(
        (gl_VertexIndex == 1) ? 2.0 : 0.0,
        (gl_VertexIndex == 2) ? 2.0 : 0.0
    );
    v_uv        = uv;
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
