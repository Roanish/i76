#ifndef MESHVIEW_H
#define MESHVIEW_H

/*
 * meshview.h — In-engine OEG mesh viewer
 *
 * A self-contained inner loop (same shape as gameplay_run) that loads meshes
 * through the real engine path — geo_cache_acquire -> vfs_read_file ->
 * geomesh_decode — and software-renders them as a rotating wireframe into the
 * 8-bit framebuffer, blitted via the normal Vulkan palette path.
 *
 * Controls: LEFT/RIGHT cycle assets, UP/DOWN zoom, SPACE toggles auto-spin,
 *           ESC / window-close to quit.
 *
 * Entered from main.c when the binary is run with --meshview. Requires
 * render_init() + meshcache_init() to have run first.
 */

void meshview_run(void);

#endif /* MESHVIEW_H */
