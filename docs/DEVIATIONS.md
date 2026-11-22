# Deviations from the original

Project rule: reproduce the original's method wherever possible, even where a better modern
method exists. We deviate only when the faithful method would be detrimental. Every deviation
is listed here, and the faithful behaviour stays available as an alternative behind
`--legacy` (`src/engine/fidelity.h`).

Status of the faithful alternative:
- **legacy: yes**: implemented, selected by `--legacy`.
- **legacy: planned**: not implemented yet; tracked here until it is.
- **legacy: n/a**: development tool only, not part of the game.

Unconfirmed reverse-engineering is marked `???` (see docs/REVERSING.md).

## Rendering

### Roads drawn into a depth buffer, not the painter's sort
- **Original:** the terrain and the road quads go into one depth-bucketed painter's list
  (FUN_00498dc0). A road quad sorts by its nearest corner, so it draws over the ground beneath
  it. There is no depth test between them.
- **Ours:** a depth-buffered renderer. Each road quad is clipped exactly to the terrain
  triangles, so it is coplanar with the ground, and drawn with a small depth bias
  (`render_3d_draw_decal`). The result looks the same, without depth fighting.
- **Why:** a painter's sort means replacing the whole renderer, and plain coplanar polygons
  fight in a depth buffer.
- **Where:** `worldview.c` `road_tri_clip`, `render_vk.c` decal bias.
- **legacy: planned.** It needs a faithful painter's-sort renderer.

### Road quads beyond 450 are drawn
- **Original:** a road quad whose centre is deeper than 450 in camera space is skipped
  (FUN_00494420, 0x004c4f68).
- **Ours:** every quad is drawn.
- **Why:** our view distance is longer than the original's, which is 150 plus fog. With the
  cut, roads would vanish while the terrain stays visible.
- **Where:** `worldview.c` `road_quad_tex`.
- **legacy: yes.**

### Road quad shape follows the terrain
- **Original:** each road quad is one flat 4-vertex polygon between two ribs.
- **Ours:** each quad is split along the terrain triangles under it, so it bends with the
  ground. The corner positions, UVs, textures and lighting are the original's.
- **Why:** same reason as the depth-buffer entry above. A flat quad sinks into or floats above
  a depth-tested ground.
- **legacy: planned**, together with the painter's-sort renderer.

### Missing road texture falls back to flat asphalt
- **Original:** the game stops with "Road texture not found, aborting".
- **Ours:** logs the missing texture and draws flat asphalt.
- **Why:** an abort is detrimental to development and to mods.
- **legacy: planned.** Abort under `--legacy`.

### Software renderer: road lifted 0.05
- The development software rasteriser has no depth bias, so its road is lifted 0.05 above
  the terrain.
- **legacy: n/a.** The software rasteriser is a development fallback, not the original's
  renderer.

### Global lighting per pixel
- **Original:** intensity is computed per vertex and mapped to one of 32 .lum shade rows per
  span.
- **Ours:** the same model and inputs, but evaluated per pixel, with the brightness curve
  (measured from the .lum) blended between rows instead of stepping through 32 bands.
- **Where:** `mesh.frag`.
- **legacy: planned.** Per-vertex intensity with banded rows.

### Night terrain is dimmed
- **Original:** at night the sky is black but the terrain keeps full brightness, so the game
  stays playable without dynamic lights.
- **Ours:** the terrain is dimmed by the phase intensity. Night visibility is to come from
  dynamic lights (headlights, buildings), which are not implemented yet.
- **legacy: planned.**

### View distance and fog
- **Original:** far plane 150 (or the mission's WDEF value), with fog.
- **Ours (world viewer):** a long draw distance and no fog.
- **legacy: planned.**

### Near plane 0.25
- **Ours:** a fixed 0.25.
- **Original:** the value is not yet found ???. This entry stays until it is confirmed.

## Simulation and input

### Frame-rate independence (planned)
- **Original:** no frame cap; dt is clamped to [0.001, 0.2] s. The camera steps per frame,
  and the keyboard axis stepper rounds dt × 182 down to whole steps, so above about 182 fps
  keys stop moving (docs/REVERSING.md "Frame timer").
- **Ours:** currently faithful. A frame-rate-independent version is planned for the modern
  mode.
- **legacy: will be the current behaviour.**

### Gameplay frame loop
- `loop.c` is a placeholder with a fixed 60 Hz step. The original runs variable dt with
  physics sub-steps (docs/REVERSING.md "Frame timer"); the world viewer already drives the
  vehicle simulation that way. The loop is replaced when gameplay moves onto the simulation.
- **legacy: n/a** (temporary).

### World-viewer keys
- The development viewer binds TAB, SPACE, L, O and others, some of which clash with the
  original bindings (TAB = reverse_direction, SPACE = weapon_fire, L = weapon_link).
- **legacy: n/a** (development tool).

## Additions (not deviations)

- **Mod layer:** presentation-only overrides (docs/MODDING.md). Faithful run: `--no-mods`.
