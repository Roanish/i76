#ifndef SCENEVIEW_H
#define SCENEVIEW_H

/*
 * sceneview.h — in-engine world scene viewer.
 *
 * Loads a mission file's placed objects (scene.c) and software-renders them as
 * wireframes at their real world transforms, viewed through an orbit camera,
 * blitted via the normal Vulkan 8-bit path. Each object's mesh is resolved
 * through the PIX/PAK index (pix.c) exactly as the original engine does.
 *
 * Controls: LEFT/RIGHT orbit, UP/DOWN zoom, SPACE height scale, P heightfield
 * orientation, ESC quit.
 *
 * Entered from main.c with --sceneview [mission]. Requires render_init,
 * vfs_init, pix_init and meshcache_init to have run first.
 */

void sceneview_run(const char *mission_path);

#endif /* SCENEVIEW_H */
