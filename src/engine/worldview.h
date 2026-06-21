#ifndef WORLDVIEW_H
#define WORLDVIEW_H

/*
 * worldview.h — solid 3D scene viewer (free-fly camera).
 *
 * The real-3D counterpart to sceneview.c's CPU wireframe: it loads the same
 * mission Scene, uploads every placed object and terrain patch as a solid mesh,
 * and renders them through the Vulkan world pass (render_3d_*). A free-fly
 * camera lets you move through the scene to judge placement and alignment.
 *
 * Renders through either backend (software CPU rasterizer or hardware Vulkan
 * pass); TAB toggles live for an A/B of the same scene and camera. `mode` is
 * the initial backend.
 *
 * Controls: WASD move, Q/E down/up, arrows look, Shift boost, P terrain height
 * scale, TAB toggle SW/HW, ESC quit.
 */

#include "render/render.h"   /* RenderMode */

void worldview_run(const char *mission_path, RenderMode mode);

#endif /* WORLDVIEW_H */
