#ifndef WORLDVIEW_H
#define WORLDVIEW_H

/*
 * worldview.h — 3D world viewer and driving test.
 *
 * The real-3D counterpart to sceneview.c's CPU wireframe: it loads the same
 * mission Scene (objects, terrain, roads) and renders it through either backend
 * (software rasteriser or Vulkan); `mode` is the initial backend. Fly mode is
 * a free camera; drive mode runs the vehicle simulation on the original key
 * bindings. The keys for each mode are listed on screen and in the README.
 */

#include "render/render.h"   /* RenderMode */

void worldview_run(const char *mission_path, RenderMode mode);

#endif /* WORLDVIEW_H */
