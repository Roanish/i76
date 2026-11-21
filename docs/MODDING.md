# Modding

Mods change how the game **looks**: textures, models and other graphics.
They never change physics or gameplay. The engine enforces this: simulation
data (vehicle definitions, mission files, terrain heights, mount points,
handling) is always read from the original game archives, and mod files are
only visible to the renderer.

## Folder layout

```
<game folder>/mods/
    load_order.txt          (optional)
    hd_wheels/
        WA11RWL3.geo
        textures/DS11BDY1.MAP
    my_paint_job/
        stag1.vtf
```

- Each mod is one folder inside `mods/`.
- A file replaces the original asset with the **same file name**. Case is
  ignored, and subfolders inside a mod are only for your own organisation:
  `hd_wheels/textures/DS11BDY1.MAP` replaces `ds11bdy1.map`.
- A replacement wins over the original even if it's in a different format.
  If the game has `foo.vqm` and your mod ships `foo.map`, yours is used.

## Load order

`mods/load_order.txt` lists mod folder names, one per line. The first line has
the highest priority. Blank lines and lines starting with `#` are ignored.
Mods not listed are not loaded.

Without `load_order.txt`, every folder in `mods/` is loaded, in alphabetical
order. When two mods supply the same file, the higher-priority one wins and
the log says which file was shadowed.

## Command line

| Option | Effect |
|---|---|
| `--mods <folder>` | use a different mods folder (default: `<game folder>/mods`) |
| `--no-mods` | ignore all mods and run on the original assets |

## What can be replaced today

| Asset | Files |
|---|---|
| Textures | `.map`, `.vqm` (+ its `.cbk` codebook), `.m16`, `.tmt` |
| 3D models | `.geo` (original OEG mesh format) |
| Vehicle paint jobs | `.vtf` |
| Palettes | `.act` |
| Screens | `.pcx` |
| Fonts | `.fnt` |

Only the original file formats are supported so far. Modern formats
(e.g. PNG textures at higher resolution, glTF models) are planned.

The startup log lists every active mod and every replacement it supplies
(`[mods] ...` lines).
