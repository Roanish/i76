# Interstate '76 Nitro Pack — Reversing Notes

Running notes on the original binary. Updated as we discover things.
Cross-reference with source comments which often go into more detail.

---

## NOTATION KEY

```
???  = a GUESS. No hard evidence. Something made it seem plausible but
      it has NOT been confirmed by actually reversing that function.
      Every ??? has a "WHY GUESSED" note explaining the reasoning, and
      a "CONFIRM BY" note explaining what would prove or disprove it.
```

---

## Binary Info

- **Game**: Interstate '76 Nitro Pack (Activision, 1997)
- **Platform**: Win32 x86
- **Compiler**: MSVC
  - CONFIRMED by: CRT startup stub pattern (`__set_app_type`, `initterm`,
    `__getmainargs`) which is specific to the MSVC runtime.

---

## Entry Points

### `entry` (CRT startup stub)
Standard MSVC CRT stub. Not interesting beyond confirming:
- `__set_app_type(2)` → GUI application (not console)
  - CONFIRMED by: `__set_app_type` takes an int; 2=GUI, 1=console.
    This is documented MSVC CRT behavior.
- Calls WinMain equivalent at `FUN_00431760`
  - CONFIRMED by: four arguments match WinMain signature exactly
    (HINSTANCE, int, char*, uint show cmd)
- Normal `initterm` / `__getmainargs` / command line parsing sequence
  - CONFIRMED by: named CRT symbols visible in decompilation

### `FUN_00431760` → renamed `Fun_Real_Entry`
The actual WinMain. Four arguments:
```
HINSTANCE param_1   hInstance
int       param_2   hPrevInstance (always 0 in Win32 — Win16 holdover)
char*     param_3   lpCmdLine
uint      param_4   nCmdShow (from STARTUPINFO or default 10)
```
CONFIRMED by: called from CRT stub with GetModuleHandleA(NULL) as
param_1, hardcoded 0 as param_2, parsed cmdline pointer as param_3,
STARTUPINFO.wShowWindow as param_4. This is the standard WinMain call.

---

## Key Globals

| Original Name    | Address      | Our Name         | Status | Notes |
|------------------|--------------|------------------|--------|-------|
| `DAT_004f30cc`   | `0x004f30cc` | `g_gamestate`    | CONFIRMED | Main state machine. Set to 5 before main loop; set to 6 by game_session_run before ShellMain. |
| `DAT_00526a44`   | `0x00526a44` | `g_exit_code`    | CONFIRMED | Set from WM_QUIT wParam. Passed to exit(). |
| `DAT_00653c18`   | `0x00653c18` | (shutdown flag)  | CONFIRMED | Set to 1 when WM_QUIT (0x12) received. Checked in every inner loop before PeekMessage. |
| `DAT_00653f78`   | `0x00653f78` | (nitshell handle)| CONFIRMED | Result of `LoadLibraryA("NITSHELL.DLL")`. Passed to `FreeLibrary` on exit. |
| `DAT_00653fa0`   | `0x00653fa0` | `pool_primary`   | CONFIRMED | Return of FUN_004a3c00(0x1a5e0, 0x40000). NULL check → fatal error. |
| `DAT_00653fa4`   | `0x00653fa4` | `pool_secondary` | CONFIRMED | Return of FUN_004a3c00(0x80000, 0x80000). NULL check → fatal error. |
| `DAT_00653f6c`   | `0x00653f6c` | (pool subregion) | CONFIRMED | `DAT_00653fa0 + 54000`. Sub-region of primary pool. |
| `DAT_00526758`   | `0x00526758` | (scenario name)  | CONFIRMED | Scenario/map name string. First char checked ('p'). Compared with "mp","ms","sp" prefixes. |
| `DAT_00526a60`   | `0x00526a60` | ???              | ??? | If nonzero, skips normal cmdline parsing and jumps to LAB_00431e97. WHY GUESSED: jump skips multiplayer flag setup. CONFIRM BY: reverse cmdline to see what sets it. |
| `DAT_00526a6c`   | `0x00526a6c` | `g_window_active`| CONFIRMED | Window focus/active flag. Set to 1 on WM_ACTIVATE(active), 0 on WM_ACTIVATE(deactivate). Previously mislabelled "demo mode" — that was wrong. Guards rendering in WM_PAINT and priority in activate handlers. |
| `DAT_00503e94`   | `0x00503e94` | `g_input_mode`   | CONFIRMED | Input/mode selector. 0x01=normal, 0x02=keyboard-only, 0x10=full mouse+KB, 0x20=video playback. Gates all input dispatch in window_proc. `& 0x3e` = any active input mode. |
| `DAT_00653f60`   | `0x00653f60` | (blit fn ptr)    | CONFIRMED | Render/blit function pointer — called during WM_PAINT as `(*DAT_00653f60)(&DAT_00653b40)`. |
| `DAT_00653bb4`   | `0x00653bb4` | (screen_w)       | CONFIRMED | Screen/window width. Used in WM_SIZE to compute centering offset. |
| `DAT_00653bb8`   | `0x00653bb8` | (screen_h)       | CONFIRMED | Screen/window height. Used in WM_SIZE to compute centering offset. |
| `DAT_004f72c0`   | `0x004f72c0` | `g_keymap`       | CONFIRMED | Virtual key mapping table. `g_keymap[VK_code]` (short) maps Windows VK codes to game key codes. 0x1b=ESC, 0x1ff=game-specific key. |
| `DAT_00534948`   | `0x00534948` | (lbutton_down)   | ??? | Set to 1 on WM_LBUTTONDOWN when not in video/mouse mode. WHY GUESSED: only write is in that handler. CONFIRM BY: find reader. |
| `DAT_00526a38`   | `0x00526a38` | ???              | ??? | If nonzero, calls FUN_0048bc20("RASTER", ...). WHY GUESSED: "RASTER" string suggests software rasterizer mode. CONFIRM BY: reverse FUN_0048bc20. |
| `DAT_005fb0e0`   | `0x005fb0e0` | (video mode byte)| CONFIRMED | Compared to 6 and 7 for font selection. Cast to `& 0xff` when stored into DAT_00653fe0. |
| `DAT_00653b40`   | `0x00653b40` | (render surface) | CONFIRMED (name only) | Large struct, `0x10b * 4 = 1068 bytes`. Copied from FUN_00484960 result. Passed to nearly every subsystem. Internal layout unknown — see Render Surface Struct section. |
| `DAT_00653bfc`   | `0x00653bfc` | (main HWND)      | ??? | Passed to FUN_004a1660 (video player) and checked before DestroyWindow. WHY GUESSED: DestroyWindow call and HWND comparison patterns. CONFIRM BY: reverse FUN_00484960 to see what offset into DAT_00653b40 this is and what type it is. |
| `DAT_004f3704`   | `0x004f3704` | ???              | ??? | If nonzero, triggers a 4-call render sequence (f3c, ecd0, f40, f60). WHY GUESSED: always tested before palette/flip calls. CONFIRM BY: find what writes this value and why. |
| `DAT_00526a5c`   | `0x00526a5c` | (font handle)    | ??? | Return of FUN_0046d440. Passed to FUN_0046d650. WHY GUESSED: called after FUN_0049ac10 which takes a font filename. CONFIRM BY: reverse FUN_0046d440. |
| `DAT_005e5cd8`   | `0x005e5cd8` | (global heap)    | CONFIRMED | Windows HANDLE passed to every HeapAlloc/HeapFree in the ZFS layer. The process heap. |
| `DAT_0052fc40`   | `0x0052fc40` | `g_object_heap`  | CONFIRMED | Mesh-cache **data heap** — holds the decoded GEO mesh buffers (node+0x0c points here). HeapCreate'd by obj_system_init, RAM-sized (g_object_heap_size). g_object_heap_size tracks free space (`-= HeapSize` on alloc, `+= HeapSize` on evict). See Object / Geometry Mesh Cache section. |
| `DAT_0052fc44`   | `0x0052fc44` | `g_object_heap2` | CONFIRMED | Mesh-cache **node heap** — holds the ~0x20-byte cache node structs. `HeapCreate(0,0,0)` (growable). Confirmed by `HeapFree(g_object_heap2, node)` in geo_build_mesh eviction. |
| `DAT_0052bcd0`   | `0x0052bcd0` | `g_obj_buckets`  | CONFIRMED | Mesh cache indexed **by data-buffer pointer**: 2029 (0x7ed) heads [0x52bcd0, 0x52dc84). Bucket = `(ptr*0x6cd+0xaab)%0x7ed`; chain via node+0x10. (Better name: g_meshcache_by_ptr.) Zeroed by obj_system_init, walked by mem_prefetch_objects. |
| `DAT_0052dc84`   | `0x0052dc84` | (inter-table dword) | ??? | Lone dword between the two cache tables; skipped by both zeroing loops in obj_system_init. CONFIRM BY: find xrefs. |
| `DAT_0052dc88`   | `0x0052dc88` | `g_obj_buckets2` | CONFIRMED | Mesh cache indexed **by name**: 2029 heads [0x52dc88, 0x52fc3c). Key = 8-byte name at node+0x00, case-insensitive (`& 0xdfdfdfdf`); bucket = `(((hi^lo)&0xdfdfdfdf)*0x6cd+0xaab)%0x7ed`; chain via node+0x14. (Better name: g_meshcache_by_name.) Zeroed by obj_system_init. |
| `DAT_0052fc48`   | `0x0052fc48` | `g_obj_list2`    | CONFIRMED | Mesh-cache **LRU list head** (oldest entry — evicted first). Doubly-linked: node+0x18 next, node+0x1c prev. (Better name: g_meshcache_lru_head.) Confirmed by geo_build_mesh eviction. mem_prefetch_objects also walks it. |
| `DAT_0052fc4c`   | `0x0052fc4c` | `g_obj_list2_tail` | CONFIRMED | Mesh-cache **LRU list tail** (newest). Set to 0 when the list empties on eviction. (Better name: g_meshcache_lru_tail.) |
| `DAT_0052fc3c`   | `0x0052fc3c` | `g_has_current_tex` | CONFIRMED | Flag: a "current texture" is set in g_current_tex_name. Used by geo_build_mesh face-texture binding. |
| `DAT_0052fc50`   | `0x0052fc50` | `g_current_tex_name` | CONFIRMED | 16-byte current texture name (fc50/54/58/5c). Compared via _stricmp during mesh face decode; FUN_004679b0 validates it. |
| `DAT_00652784`   | `0x00652784` | `g_object_heap_size` | CONFIRMED | Free-space counter for g_object_heap. Initial: dwTotalPhys > 48 MB → 2,000,000; ≤ 48 MB → 1,300,000. |
| `DAT_00526a50`   | `0x00526a50` | `g_geo_log_file`  | CONFIRMED | FILE* debug log handle. When non-null, geo_cache_acquire fprintf's "<name> <size>" for each mesh loaded. |
| `DAT_00526a54`   | `0x00526a54` | `g_geo_load_depth` | ??? | Counter incremented on entry to geo_cache_acquire's load path, decremented on exit. Re-entrancy / in-flight-load guard. CONFIRM BY: find readers. |
| `DAT_00529bf0`   | `0x00529bf0` | `g_objmodel_registry` | CONFIRMED | Object→model registry: 2029-bucket hash keyed by **object pointer** (`(objptr*0x6cd+0xaab)%0x7ed`), chain via entry+0x38. Each entry is a 0x3c-byte model record (see Object Model Registry). Distinct from the mesh cache (g_obj_buckets). |
| `DAT_0052bba4`   | `0x0052bba4` | `g_objmodel_heap` | CONFIRMED | Heap for the 0x3c-byte model registry entries. HeapAlloc'd in obj_model_clone. |
| `DAT_0052bba8`   | `0x0052bba8` | `g_objmodel_node_heap` | CONFIRMED | Heap for the 0x74-byte model sub-part nodes (singly-linked via +0x70). |
| `DAT_0052fc70`   | `0x0052fc70` | `g_arr_buckets`  | CONFIRMED | Typed-array object hash table — 109 linked list heads (table ends at 0x52fe24). Each node+0x10 → buffer descriptor {count, element_size_with_flags}. Walked by mem_prefetch_arrays. |
| `DAT_0052fc6c`   | `0x0052fc6c` | `g_arr_list2`    | ??? | Separate typed-array list head (node stride +0x1c). Walked by mem_prefetch_arrays after bucket sweep. CONFIRM BY: find what writes this. |
| `DAT_005e5ce0`   | `0x005e5ce0` | (error buffer)   | CONFIRMED | Global char[] error message buffer. Every ZFS/VFS error sprintf's into here before returning 0/NULL. |
| `DAT_005e5ee0`   | `0x005e5ee0` | `g_vfs_file_table`  | CONFIRMED | Pointer to sorted VFS file entry array. bsearched by vfs_lookup. Stride 0x30. |
| `DAT_005e5ee4`   | `0x005e5ee4` | `g_vfs_file_count`  | CONFIRMED | Count of entries in g_vfs_file_table. |
| `DAT_005e6120`   | `0x005e6120` | (root path table)   | CONFIRMED | Table of root path strings. Stride 0x300 = 768 bytes/entry. Indexed by VFSSource.base_path_idx. |
| `DAT_005f9b24`   | `0x005f9b24` | (drive override)    | ??? | When nonzero, path-building uses get_drive_letter() instead of root path table. CONFIRM BY: find what writes it. |
| `g_vfs_source_count` | (TBD)   | `g_vfs_source_count` | CONFIRMED | Count of VFSSource entries. |
| `g_vfs_source_names` | (TBD)   | `g_vfs_sources`     | CONFIRMED | Array of VFSSource structs (268 bytes each). NOT just names — full source entries incl. type, base_path_idx, zfs_handle. |
| `g_vfs_source_types` | (TBD)   | (field ptr)         | CONFIRMED | Points to VFSSource[0].type — the type field of the first source. Not a separate array. |
| `g_file_cache_buckets` | (TBD) | `g_file_cache_buckets` | CONFIRMED | Hash table, 2009 buckets. HeapAlloc'd from g_cache_heap. Each bucket = linked-list head of cache entries. |
| `DAT_006520d4`  | `0x006520d4` | `g_cache_heap`          | CONFIRMED | Private Win32 heap handle. HeapCreate'd by vfs_cache_init, HeapDestroy'd by vfs_cache_destroy. Sized by RAM. |
| `DAT_006520a0`  | `0x006520a0` | `g_cache_heap_size`     | CONFIRMED | Total private heap size (set by vfs_cache_init based on RAM). Decremented by bucket array size after alloc. |
| `DAT_006520cc`  | `0x006520cc` | `g_lru_tail`            | CONFIRMED | Oldest entry on the LRU free list (evicted first). Set to first freed entry; only updated when a freed entry has no successor. |
| `DAT_006520d0`  | `0x006520d0` | `g_lru_head`            | CONFIRMED | Most recently freed entry on the LRU free list. Updated every time an entry's refcount hits 0. |
| `DAT_00590a64`  | `0x00590a64` | `g_pak_entries`         | CONFIRMED | Sorted `PakEntry[]` array built by `pix_init`. Stride 0x1c. bsearched by geo_name — see PIX/PAK section. |
| `DAT_00590a68`  | `0x00590a68` | `g_pix_file_names`      | CONFIRMED | Array of 24-byte records (16-byte `.pak` filename + 8 bytes zero), one per .pix file found by glob. Built in pix_init pass 1. |
| `DAT_00590a6c`  | `0x00590a6c` | `g_pix_file_count`      | CONFIRMED | Count of .pix files indexed in `g_pix_file_names`. |
| `DAT_00590a70`  | `0x00590a70` | `g_pak_entry_count`     | CONFIRMED | Total PakEntry count across all .pix files. Size of `g_pak_entries`. |
| `DAT_00526a3c`  | `0x00526a3c` | `shell_window_proc`     | CONFIRMED | `GetProcAddress(nitshell, "ShellWindowProc")`. Stored at session start. |
| `DAT_00653f4c`  | `0x00653f4c` | (gfx_init fn ptr)       | CONFIRMED | Function pointer — `(*DAT_00653f4c)(&DAT_00653b40, mode)`. Called twice in game_session_run with mode=0 then mode=uVar6. Init fails → fatal_error. |
| `DAT_00653b20`  | `0x00653b20` | `shell_result`          | CONFIRMED | ShellMain return code: 5=multiplayer session, 2/4=singleplayer, 3=MP singleplayer, 0xff=quit without starting. |
| `DAT_00653b29`  | `0x00653b29` | (shell scenario name)   | CONFIRMED | Scenario name string set by ShellMain. Copied to DAT_00653ff0 for session results 2/3/4. |
| `DAT_00653ff0`  | `0x00653ff0` | (session scenario name) | CONFIRMED | Destination buffer for scenario name after ShellMain returns. Passed into session setup. |
| `DAT_004fc498`  | `0x004fc498` | (hi-res flag)           | ??? | If nonzero, video mode flags = 0x617700; else 0. WHY GUESSED: only use is to gate the high video mode value passed to ShellMain and gfx_init. CONFIRM BY: find what writes it. |
| `DAT_00653b24`  | `0x00653b24` | ???                     | ??? | Passed as arg 8 to ShellMain. WHY GUESSED: adjacent to shell_result (DAT_00653b20) in the render surface struct region. CONFIRM BY: reverse ShellMain interface. |
| `DAT_004f30c8`  | `0x004f30c8` | ???                     | ??? | Passed as arg 7 to ShellMain. Adjacent to g_gamestate (0x004f30cc). CONFIRM BY: reverse ShellMain interface. |

---

## Game State Machine

`DAT_004f30cc` / `g_gamestate` values, reverse engineered from
Fun_Real_Entry control flow:

| Value  | Enum Name      | Evidence |
|--------|----------------|----------|
| `5`    | GS_GAMEPLAY    | CONFIRMED — set at 0x00431dcc. Inner `while(DAT_004f30cc == 5)` is the hot gameplay loop. |
| `1`    | GS_FRONTEND    | CONFIRMED — triggers video mode reset to 0, plays outro SMK (DAT_00653f90), reloads vehscn.vsf |
| `0`    | GS_LOBBY       | CONFIRMED — runs FUN_0049e900(). Inner loop runs `while(state==0)`. |
| `2`    | GS_MAINMENU    | ??? — set when FUN_0049e900() returns 0 (failure). WHY GUESSED: "fallback from lobby" suggests menu. CONFIRM BY: reverse FUN_0049e900 to see what state 2 actually does. |
| `3`    | GS_ENDSESSION  | CONFIRMED — set when 10-second grace timer elapses after session-end condition. |
| `7`    | GS_MISSIONEND  | CONFIRMED — triggers DAT_00653b20 check and path copy into DAT_00653ff0, then resets to state 5. |
| `8`    | GS_MP_FOLLOWON | CONFIRMED — set when state==7 AND scenario prefix == "mp" (_strnicmp check at ~0x004337xx). |
| `0xb`  | GS_LOBBY_ALT   | CONFIRMED — set when state==1 AND DAT_00534080 != 0. Treated same as state 0 in else-if branch. |
| `0xff` | GS_QUIT        | CONFIRMED — local_3c return value of -1 (0xff when cast to char) triggers full shutdown sequence. Not stored in DAT_004f30cc directly — checked as `(char)local_3c == -1`. |

---

## Key Functions

### Notation

```
CONFIRMED = we have seen enough of the function's call sites and/or
            internal structure to be confident of its role.

??? = we are GUESSING based on context clues. See WHY GUESSED and
      CONFIRM BY notes for each.
```

### Confirmed

| Address        | Our Name              | Evidence |
|----------------|-----------------------|----------|
| `FUN_00431760` | `Fun_Real_Entry`      | WinMain. Confirmed by CRT stub call. |
| `FUN_00430eb0` | `game_session_run()`  | Called in outer loop with (hwnd, cmdline, flag, config[]). Returns state code used as `local_3c`. The session runner — everything else in Fun_Real_Entry wraps it. |
| `FUN_004a1660` | `video_play()`        | Takes (hwnd, smk_filename). Returns nonzero if file found. Called with literal .smk filenames. |
| `FUN_004a1c20` | `video_frame_tick()`  | No args. Returns 0 when video done. Called in while() loop after video_play. |
| `FUN_004a1c60` | `video_stop()`        | No args. Called when `DAT_00503e94 == 0x20` inside video loop. |
| `FUN_004a3c00` | `vmem_alloc()`        | Args: (commit_size, reserve_size). Returns ptr, NULL on fail. Called exactly twice with known sizes. |
| `FUN_0043a6d0` | `obj_system_init()`   | No args. Mesh-cache init (full body confirmed): reads MEMORYSTATUS; zeroes both 2029-head cache tables (g_obj_buckets, g_obj_buckets2); clears LRU head/tail; sizes g_object_heap_size by RAM (2.0M / 1.3M); `HeapCreate`s g_object_heap (data) + g_object_heap2 (nodes). Returns false on data-heap failure, else `(g_object_heap2 == NULL)`. |
| `FUN_0043a770` | `geo_cache_acquire()` | Args: (name_ptr → 8-byte mesh name). The mesh-cache acquire entry point and the only caller of geo_build_mesh. Uppercases the name, looks it up in g_obj_buckets2; on hit bumps refcount (+0x08) and pulls the node off the LRU free-list if idle; on miss loads via vfs_lod, allocates a 0x20-byte node from g_object_heap2, decodes via geo_build_mesh, and links it into both hash tables. Returns the mesh data ptr (0 on load failure). Full body confirmed — see Object / Geometry Mesh Cache. |
| `FUN_0043a9a0` | `geo_cache_release()` | Args: (mesh data ptr). Mesh-cache deref — looks node up via g_obj_buckets (by-ptr), refcount--, appends to LRU tail on 0. Confirmed via obj_model_clone / obj_model_set_state. Completes the mesh cache. |
| `FUN_00438d00` | `obj_model_set_variant()` | Args: (object ptr, variant index). Full body confirmed. Sets the model's **variant axis** (record+0x08 = which sub-part node). Walks the node list to `index`, uses the current slot (record+0x0c) to pick a 0x10-byte name; if it differs from the bound name, releases old / acquires new mesh, stores in record+0x04 and object+0x5c, then FUN_0043ce90(obj) rebuilds render state. 3rd of 4 geo_cache_acquire callers. |
| `FUN_00438e10` | `obj_model_set_slot()` | Args: (object ptr, slot index). Full body confirmed. Sets the model's **slot axis** (record+0x0c). Uses the current variant (record+0x08) to find the node, picks name at `slot*0x10`; same release/acquire/rebuild if changed. Keeps a 1-deep undo cache (record+0x10 = prev slot index, record+0x24 = prev slot name) to skip re-lookup when toggling back. 4th of 4 geo_cache_acquire callers. |
| `FUN_00438a80` | `obj_model_clone()`   | Args: (src_obj ptr, dst_obj ptr). Full body confirmed. Looks up src_obj in g_objmodel_registry, allocates+inserts a new 0x3c-byte model record for dst_obj, deep-copies the src's 0x74-byte sub-part node list, copies object fields +0x84..+0xab src→dst, copies the model name, `geo_cache_acquire`s the mesh and stores it in the record (+0x04) and the object (+0x5c). Returns 1 (0 if src not registered). 2nd of 4 geo_cache_acquire callers — the object→model binding layer; entity-adjacent. See Object Model Registry. |
| `FUN_004312f0` | `assets_preload()`    | Args: (scenario/level base name). Full body confirmed. Builds a manifest filename (extension chosen by display_ready: D3D vs software), loads it, and walks it via asset_list_next, warming each resource cache up to a per-category memory budget. Per entry type: 'g'→geo_cache_acquire+geo_cache_release (<200K geom stop), 't'→texture_load (<500K), 'x'/'v'/'a'→texture_load+FUN_00489d40 D3D upload (<700K; a=type2,x=type1), 'p'→vfs_lod pre-fault (<1.7M). Logs cache free-space to g_geo_log_file/DAT_005b1f80. One of 4 callers of geo_cache_acquire — a preloader, NOT the entity store. |
| `FUN_0043aa60` | `geo_build_mesh()`    | Args: (geo_image ptr [magic 0x2e47454f], size). Full body confirmed. Allocates a renderable mesh buffer from g_object_heap and unpacks the GEO's vertex arrays (3×float, +0x0c/+0x10 sub-arrays), face records (0x40 + n*0x10 each, +0x14), per-face texture binding (FUN_004679b0 + g_current_tex_name) and centroids. Returns the buffer. On heap-full, runs the LRU **eviction** loop (frees victim's data from g_object_heap + node from g_object_heap2, unlinks from both hash tables + LRU list) and retries; fatal "new geometry: couldn't make room" if LRU empty. Called only by geo_cache_acquire. |
| `FUN_0041b3c0` | `sound_buffer_load()` | Args: (sound_system, sound_resource, fmt_out). DirectSound sample loader (confirmed by FourCC magics + IDirectSound/IDirectSoundBuffer vtable offsets). Checks a by-name cache (FUN_0041a6d0); if already loaded, `DuplicateSoundBuffer`s the existing buffer. Else `vfs_lod`s the file, handles a `GAS0` (0x30534147) header (skip 0x1c, copy 7-dword fmt desc to resource+0x30), parses `RIFF`/`WAVE`/`fmt `/`data`, then `CreateSoundBuffer`(dev vtbl+0xc) → `Lock`(+0x2c) → memcpy PCM → `Unlock`(+0x4c). Resource struct: +0x04 name, +0x30 fmt desc(0x1c), +0x4c/+0x50 file ptr/cursor, +0x54 IDirectSoundBuffer, +0x58 3D buffer, +0x6c refcount, +0x70 size, +0x78 flags (bit 0x200 = in-memory). g_sound_bytes_total accumulates loaded size. **Proves `vfs_lod` returns RAW file bytes — header handling is the caller's job.** |
| `FUN_004a3cc0` | `vmem_free()`         | Takes the ptr returned by vmem_alloc. Called on both pools at shutdown. |
| `FUN_00484960` | `window_create()`     | Takes (buffer, hInstance, class_name). Returns ptr. Result copied into DAT_00653b40 (1068 bytes). |
| `FUN_00488d20` | `fatal_error()`       | Takes string. Marked `/* WARNING: Subroutine does not return */` in Ghidra. Called for all fatal conditions. |
| `FUN_00470b90` | `pcx_open()`          | Takes filename string. Returns ptr to image header struct, or NULL if not found. First word = width, second = height. |
| `FUN_00471000` | `pcx_decode()`        | Takes (filename, dest_surface_ptr). Returns nonzero on success. |
| `FUN_00499b20` | `pcx_free()`          | Takes ptr returned by pcx_open. Called in `if (ptr != NULL)` guard. Also used as a general heap free throughout (same HeapFree wrapper) — seen freeing g_pix_file_names and g_pak_entries in session_init_a. |
| `audio_load_dat` | `audio_load_dat()`  | Takes filename ("engsnd.dat"). Called at session start and again mid-loop. |
| `FUN_00448620` | `is_multiplayer()`    | No args. Return value gates all network-specific code paths. Consistent pattern throughout. |
| `FUN_0042fee0` | `is_host()`           | No args. Return value gates host-only code paths. Always checked alongside is_multiplayer. |
| `FUN_00448820` | `get_session_flags()` | No args. Returns bitmask — bit values confirmed from string context (see Session End Flags). |
| `FUN_004487b0` | `get_session_score()`  | Called with no args when get_session_flags bit is set. Return value used in sprintf as %d score. |
| `FUN_0046cdb0` | `palette_find_entry()` | Args: (r, g, b) as floats 0.0–1.0. Returns palette index. Called with (0,0,0) to find black. |
| `FUN_0046c330` | `palette_set_range()`  | Args: (surface, r, g, b) — sets a range of palette entries. |
| `FUN_0046c3d0` | `palette_upload()`    | Args: (surface). Uploads palette to display. |
| `FUN_00499cd0` | `perf_marker()`       | Args: string label ("AI", "Blit"). Debug/profiling marker. Likely no-op in release. |
| `FUN_00499a00` | `display_ready()`     | No args. Returns nonzero if display initialized. Called before palette fade loops. |
| `FUN_0049ac10` | `font_load()`         | Args: (font_id, filename). Called with one of three .fnt filenames. |
| `FUN_0046d440` | `font_activate()`     | Args: (font_id from font_load). Returns handle stored in DAT_00526a5c. |
| `FUN_0046d650` | `font_free()`         | Args: handle from font_activate. Called at session start with DAT_00526a5c. |
| `FUN_00488ed0` | `screenshot_save()`   | Args: filename ("SCRDUMP.BMP"). Called when DAT_00526a40 != 0 and display ready. |
| `FUN_004bc730` | `save_path_build()`   | Args: scenario name string. Builds save file path. |
| `FUN_0049fc80` | `active_tick()`       | ??? No args. Called when g_window_active != 0 in every event loop. Previously guessed "demo_tick" based on wrong DAT_00526a6c interpretation — now known to be the "window is focused" tick. CONFIRM BY: reverse it. |
| `window_proc`  | `window_proc()`       | LRESULT (HWND, uint msg, WPARAM, LPARAM). Full Win32 WndProc. Routes messages to ShellWindowProc when GS_SHELL. Handles WM_ACTIVATE (focus/priority), WM_SIZE (viewport), WM_PAINT (blit), WM_CLOSE/WM_DESTROY (→GS_MAINMENU), 0x432 (→GS_GAMEPLAY), input dispatch via g_input_mode. Full body confirmed. |
| `FUN_0049e470` | `mouse_input()`       | Args: (msg, wParam, lParam). Mouse input dispatcher — called for WM_LBUTTONDOWN/UP when g_input_mode==0x10. |
| `FUN_004a1fe0` | `video_mouse_skip()`  | No args. Called on WM_LBUTTONDOWN when g_input_mode==0x20 (video playback). Probably skips current video. |
| `FUN_0049fdc0` | `input_escape()`      | No args. Called on ESC key (0x1b) in video mode or 0x1ff key code. Escape/skip handler. |
| `FUN_004a2410` | `video_is_playing()`  | No args. Returns int. Called on WM_ACTIVATE(deactivate) to check if a video is running. |
| `FUN_00442c20` | `input_dispatch()`    | Args: (msg, wParam, lParam). General input handler for non-video modes (g_input_mode not 2 or 0x20). |
| `FUN_00467630` | `vfs_read_file()`     | Takes filename string. Returns malloc'd buffer with decompressed file content (or NULL). Called for every asset load — PCX, VQM, CBK, etc. Cache-aware; dispatches to `vfs_read_file_impl` for the raw read + decompression. |
| `FUN_00467550` | `vfs_exists()`        | Takes filename string. Returns nonzero if file is available (ZFS or loose). |
| `FUN_004673b0` | `vfs_free()`          | Takes buffer returned by vfs_read_file. Frees it. |
| `FUN_0043b910` | `texture_load()`      | Takes filename (VQM base name). Loads VQM + CBK codebook, decodes texture, inserts into texture cache. Returns pointer to decoded texture data. |
| `FUN_0043dc10` | `vqm_decode()`        | Args: (vqm_data, cbk_data, out_buf, out_size). Decodes Vector Quantized texture into output buffer. Returns nonzero on success. Separate from LZO — this is the VQM codec. |
| `FUN_004beee0` | `vfs_enumerate_sources()` | Args: (out_buf, &count_out, &unused_out). Walks g_vfs_source_count source table entries. For each loose-dir source (type=0): FindFirstFile; for ZFS sources: nitro.zfs is skipped here (enumerated separately). Output entries stride 0x30: +0x00 filename (16B, lowercased), +0x10 source_index (4B), +0x14 unknown (28B). param_3 always set to 0. |
| `FUN_004bdab0` | `zfs_create()`        | Args: (path, xor_key). Creates a new empty ZFS archive on disk. Fails if file already exists. Writes 28-byte header (total_count=0, first_block_offset=28) + one zeroed 3604-byte block. xor_key stored in header offset 20–23. |
| `FUN_004bdcc0` | `zfs_load_dir()`      | Args: (path, FILE*). Allocates 44-byte ZFS handle, reads all directory blocks (linked by next-block offset), filters deleted entries (flags bit 0), sorts live entries by name (qsort), returns handle or NULL on error. |
| `FUN_004be000` | `zfs_find()`          | Args: (handle, name). Binary searches sorted entry array by name (16-byte key). Returns: -1=not found, flags>>8=uncompressed size (when bits 1/2 set), entry.size=stored size (uncompressed file). |
| (TBD)          | `zfs_read_alloc()`    | Thin wrapper: calls `zfs_decompress_into_impl(h, name, &size, 0, 0)`. Alloc mode. |
| (TBD)          | `zfs_decompress_into()` | Thin wrapper: calls `zfs_decompress_into_impl(h, name, &size, buf, cap)`, returns bool. |
| (TBD)          | `zfs_decompress_into_impl()` | The real read+decompress engine. bsearch by name, fread compressed data, LZO decompress if flags&6, XOR-decrypt if xor_key!=0, return buffer. Allocates if buf==NULL. |
| (TBD)          | `lzo_decompress_dispatch()` | Args: (src, src_size, entry_flags_byte, dst, dst_size). Lazy-inits LZO library on first call. Dispatches: flags&2→lzo1x_decompress, flags&4→lzo1y_decompress. Returns 1 on success, 0 on error. |
| `FUN_00444110` | `lzo_init()`          | `__lzo_init_v2(0x1000, 2, 4, 4, 4, 4, 4, 4)` — LZO library init. Version 0x1000 = LZO 1.0x. Called once from lzo_decompress_dispatch. |
| `FUN_004438f0` | `lzo1x_decompress()`  | ??? Standard LZO1X decompressor. Args: (src, src_size, dst, &dst_size, NULL). WHY GUESSED: called when flags&2; LZO1X is the most common variant. CONFIRM BY: check function signature against known LZO source. |
| `FUN_00443c88` | `lzo1y_decompress()`  | ??? Standard LZO1Y decompressor. Args: (src, src_size, dst, &dst_size, NULL). WHY GUESSED: called when flags&4; LZO1Y is the next most common variant. CONFIRM BY: same as above. |
| `FUN_00467cd0` | `pix_init()`          | No args. Builds `g_pak_entries` and `g_pix_file_names` at startup. Three passes: (1) glob `*.pix` → build pak-name array; (2) open each .pix, read entry count; (3) read each entry into `PakEntry[]`; then qsort by geo_name. Full body confirmed — see PIX/PAK section. Also called once per session inside game_session_run after ShellMain returns (when shell_result != 0xff). |
| `FUN_00430eb0` | `game_session_run()`  | Args: (param_1, param_2, param_3, param_4). Full body confirmed. Builds 32-entry ShellMain callback table on stack, calls `ShellMain` from NITSHELL.DLL, decodes result, reinits graphics, then calls per-session init chain (FUN_00468130 → FUN_00467120 → FUN_004671a0 → pix_init). Returns ShellMain result. See game_session_run section below. |
| `FUN_0042fe40` | `set_multiplayer()`   | Args: (int flag). Called with 0 at game_session_run entry (reset) and 1 for shell_result==3 (MP singleplayer). Setter counterpart to is_multiplayer(). |
| `FUN_00448630` | `set_session_mp()`    | Args: (1). Called for shell_result==5 (multiplayer). Likely sets a multiplayer-session-active flag distinct from is_multiplayer(). |
| `FUN_004486d0` | `session_mp_init()`   | Args: (param_4 — network session descriptor ptr). Called for shell_result==5. Copies network session fields into globals. |
| `FUN_00468130` | `session_init_a()`    | No args. PIX/PAK teardown — iterates g_pix_file_names, calls vfs_cache_deref on any loaded entry (entry+0x10 != 0), then frees g_pix_file_names and g_pak_entries. Runs at start of each session before pix_init rebuilds. Full body confirmed. |
| `FUN_004674a0` | `vfs_cache_deref()`   | Args: pointer to a filename string (in practice always a g_pix_file_names entry, first 16 bytes = pak filename). Hashes the filename, walks the VFS cache bucket chain via `_stricmp`, decrements refcount (short at entry+0x14). When refcount hits 0, appends to LRU free list (g_lru_head/g_lru_tail). General-purpose — works on any filename in the cache, not PAK-specific. Full body confirmed. |
| `vfs_cache_destroy` | `vfs_cache_destroy()` | No args. VFS file cache teardown — walks all 2009 hash buckets, follows each linked list calling vfs_cache_evict on every entry, resets g_file_cache_count/g_lru_tail/g_lru_head to 0, then HeapFree(bucket_array) + HeapDestroy(g_cache_heap). Full body confirmed. |
| `FUN_004671a0` | `vfs_cache_init()`    | No args. VFS file cache init — calls query_memory_status to get RAM size, sizes private heap by RAM (< 32MB→6MB, 32-64MB→7MB, ≥64MB→11MB), HeapCreate(heap), HeapAlloc(bucket_array, 0x1f64 = 2009×4 bytes), sets g_file_cache_max=2000. Full body confirmed. Already renamed in Ghidra. |
| `query_memory_status` | `query_memory_status()` | Args: pointer to 32-byte MEMORYSTATUS buffer. Wraps GlobalMemoryStatus. Return[2] (dwTotalPhys) used by vfs_cache_init to size the heap. Already renamed in Ghidra. |
| `FUN_0041b320` | (session_frame_a)     | No args. Called at game_session_run entry AND again just before return. Bracket pattern — probably a push/pop of some render or thread state. |
| `FUN_0041b350` | (session_frame_b)     | No args. Called once just before return, after the second FUN_0041b320 call. Paired with FUN_0041b320. |
| `FUN_00419bd0` | (display_toggle)      | Args: (int). Called with 1 at entry and 0 just before return. Bracket pattern — enable/disable display or event processing during shell. |
| `FUN_00443610` | (pre_shell_setup)     | No args. Called once after gfx_init succeeds but before ShellMain. |
| `FUN_004bf0e0` | `vfs_glob()`          | Args: (pattern, &state). Stateful enumerator — first call with a state struct, subsequent calls return next match. Used by pix_init with `"*.pix"`. Returns pointer to 16-byte filename buffer, NULL when exhausted. |
| `FUN_004bf910` | `vfs_open_text()`     | Args: (filename, out_buf). Opens a VFS file and returns its content as a heap-allocated text buffer. Used by pix_init to read .pix file content for sscanf parsing. |
| `FUN_004bfb70` | `vfs_free_text()`     | Args: buffer from vfs_open_text. Frees it. Likely a thin wrapper over the same allocator as vfs_free. |

### VFS Layer — Named, Bodies Not Yet Fully Reversed

These were renamed during Ghidra work; full decompilation not yet recorded here.

| Address        | Our Name              | What We Know |
|----------------|-----------------------|--------------|
| (TBD)          | `vfs_lookup()`        | CONFIRMED: bsearches g_vfs_file_table by name, selects best source via source bitmask, returns pointer to VFSSource entry in g_vfs_sources. |
| (TBD)          | `vfs_read_file()`     | Thin bool wrapper: returns `vfs_read_file_impl(...) != 0`. |
| (TBD)          | `vfs_read_file_impl()`| CONFIRMED signature: (name, &size_out, buf_or_null, capacity). Dispatches to loose-file fread or ZFS decompress path. See VFS Layer section. |
| (TBD)          | `vfs_exists()`        | Returns uncompressed file SIZE (not bool); 0 = not found. Two-level cache (vfs_fast_size_lookup + g_file_cache_buckets hash table), falls back to vfs_get_uncompressed_size. |
| `pak_find_entry` | `pak_find_entry()`  | bsearch into `g_pak_entries` (DAT_00590a64, stride 0x1c) by geo_name. Returns value at entry+0x18 (`PakEntry.frame_count`), or -1 on miss. Confirmed by pix_init analysis. |
| (TBD)          | `vfs_get_uncompressed_size()` | Path-building identical to vfs_read_file_impl. Loose: ftell. ZFS: zfs_find (returns flags>>8 or entry.size). Returns 0 on not-found. Consistent "bytes needed after load" API. |
| (TBD)          | `vfs_cache_evict()`   | Evicts an entry from the VFS read cache. |
| (TBD)          | `vfs_lod()`           | CONFIRMED body. Dispatcher: copies name (16B), `_strlwr` lowercases it, rejects names whose first 4 chars == DAT_004c7e70 (a sentinel string, likely "none"/"null" → return 0). Otherwise routes by g_vfs_prefer_loose: set → vfs_lod_cached then vfs_lod_pak; clear → vfs_lod_pak then vfs_lod_cached. Returns first non-zero. Does NOT do the file read / OEG framing itself. |
| (TBD)          | `vfs_lod_cached()`    | CONFIRMED body. The VFS file-cache **acquire** (counterpart to vfs_cache_deref). Hashes the name (`h=h*2^c`, abs, %2009), walks g_file_cache_buckets; on hit, unlinks from LRU if refcount==0 then refcount++; on miss, evicts LRU tail if full, `HeapAlloc(g_cache_heap, size+0x24)`, `vfs_read_file(name,…,entry+0x24,size)`, inserts + inits the 0x24 header. **Returns entry+0x24 = the RAW file bytes (no framing/strip).** Fatal if size+0x24 > 0x7fff7. So the OEG framing is NOT here — loose meshes come back verbatim. |
| (TBD)          | `vfs_lod_pak()`       | CONFIRMED body. PIX/PAK resolver: `bsearch(name, g_pak_entries, g_pak_entry_count, 0x1c, cmp@LAB_00468310)` → PakEntry. PakEntry+0x10 = pix-file index → g_pix_file_names record (stride 0x18). Lazily loads that .pak via `vfs_lod_cached(record+0x00)` into record+0x10 (fatal "Could not load pack %s" on fail), refcount++ at record+0x14. **Returns `loaded_pak_base + PakEntry+0x14`** — pointer to this entry's record inside the loaded PAK. This is the source of the records geo_build_mesh parses. |

### Unknown — Need Investigation

| Address        | Why It Matters | Clues We Have |
|----------------|----------------|---------------|
| `FUN_0049ae20` | Very early init, before window | Takes 32-byte stack buffer (local_53c). No other clues. |
| `FUN_00499a10` | Early init | No args. Called before strncpy of cmdline. |
| `FUN_00499a60` | Called twice: early init AND end of each session | No args. The dual call site is suspicious — may be a reset function. |
| `FUN_0049af60` | After srand() | No args. Timing-related? ??? WHY GUESSED: placed right after RNG seed. CONFIRM BY: reverse it. |
| `FUN_0046d010` | Before LoadLibrary | No args. |
| `FUN_0046ea40` | After LoadLibrary, before window | No args. |
| `FUN_0049e290` | Called twice: after window init AND after each session | No args. Like FUN_00499a60, dual call site — may be a reset or clear. |
| `FUN_0042a820` | After PCX load, before valloc | No args. |
| `FUN_00468810` | After valloc | No args. |
| `FUN_0049a960` | After valloc | No args. |
| `FUN_0046d960` | After valloc | Args: render surface. |
| `FUN_00419680` | After FUN_0046d960 | Args: HWND (DAT_00653bfc). |
| `FUN_004a1630` | After FUN_00419680 | Args: HWND (DAT_00653bfc). |
| `FUN_00443490` | Called twice in session setup | No args. |
| `FUN_004bc6e0` | First call in session teardown | No args. |
| `FUN_00442fa0` | After outro video | No args. |
| `FUN_00418e80` | Args: (-1, 0) | Called at two teardown points. |
| `FUN_0044dee0` | Args: (0x40000) — size hint | Returns ptr. NULL = OOM → MessageBox + exit. Not the same as vmem_alloc (different size, different error path). |
| `FUN_00433916` | Called just before return in exit path | No args. Last thing before ExceptionList restore. |
| `FUN_0043afb0` | `mem_prefetch_objects()` | No args. Memory page prefetch — walks all 2029 g_obj_buckets linked lists + g_obj_list2, calls HeapSize on each node's data ptr (node+0x0c), then touches each 4KB page to avoid mid-frame page faults. Ghidra lost the page-touch memory access; shows only a countdown loop. Called at top of gameplay inner loop. |
| `FUN_0043c180` | `mem_prefetch_arrays()` | No args. Same page-prefetch pattern as mem_prefetch_objects but for typed element arrays. Walks 109-entry g_arr_buckets table + g_arr_list2. Size = P[0]*((P[1]<<3)>>3) where node+0x10 → {count, element_size_with_flags}. Skips entries where size==-8 or size<=-9 (sentinels). Countdown loop again has lost page-touch dereference. Full body confirmed. |
| `FUN_0040ce70` | Top of gameplay inner loop | No args. |
| `FUN_00441370` | Common in event loops | No args. Appears in both gameplay and lobby loops. Input? ??? WHY GUESSED: position in loop alongside PeekMessage. CONFIRM BY: reverse it. |
| `FUN_004a24a0` | Controls video-playing branch in gameplay loop | No args. Returns int. Nonzero → calls FUN_004a24c0 and plays a video. |
| `FUN_004a24c0` | Feeds into video_play() | No args. Return value passed as filename to video_play. ??? WHY GUESSED: return value used directly as smk filename arg. CONFIRM BY: what does it return? |
| `FUN_004198b0` | Args: (1) | Fallback when `DAT_00503e94 != 0x20`. Called in video loops. |
| `FUN_004a2440` | On window activate | No args. Returns int. Called during WM_ACTIVATE(active). Nonzero + (g_input_mode & 0x3e)==0 → calls FUN_00419030. |
| `FUN_00419030` | On window activate, non-video | No args. Called on WM_ACTIVATE when video not active and not in video mode. Probably restores DirectDraw surfaces. |
| `FUN_00418ff0` | On window deactivate, non-video | No args. Called on WM_ACTIVATE(deactivate) when video not playing. |
| `FUN_00430860` | On window activate, input mode 0x10 | Args: (&DAT_004f36c8). Called on WM_ACTIVATE for g_input_mode==0x10. |
| `FUN_0049da10` | On window activate, input mode 0x10 | No args. Paired with FUN_00430860 on WM_ACTIVATE. |
| `FUN_0046d1c0` | `palette_blackout()` | No args. CONFIRMED. Sets _DAT_00591284=1, zeroes a 256×3 palette (all black), calls SetPalette (DAT_00653f48) on DAT_00653b40 — blanks the screen to black for a load transition. NOT scene/placement. |
| `FUN_0049f7e0` | `scenario_classify()` | Args: scenario name. CONFIRMED. `sscanf("%1s%2d")` → prefix char + number, resolves a small code into `DAT_00503d78` (g_scenario_category): prefix==DAT_00503d80→1, ==DAT_00503d7c→2, else a `{char,num,code}` table at DAT_00503c8c. Classifier only — NOT placement. |
| `FUN_004bd1b0` | `mission_file_load()` | Args: scenario name. CONFIRMED (strings "Cannot locate Mission file %s", "Mission file %s not found"). Parses name → mission number (DAT_005f9b2c = atol) + type (DAT_005f9b30 by 1st letter: m/n→1, t→2, s/p→3); appends default mission ext (DAT_00506528) if none; resolves path via `FUN_004bf370` (mission_file_locate); loads via `vfs_lod` into DAT_005e5ccc (end=DAT_005e5cd0); parses TWO tagged chunks via `FUN_004bccb0(buf, tag, code, 0, name, 1, end)` — tags DAT_005061e8 (code 2) then DAT_00506208 (code 7). **`FUN_004bccb0` is the mission-chunk parser — the likely home of object/vehicle PLACEMENT.** |
| `FUN_004bf370` | `mission_file_locate()` ? | Args: mission name. Returns resolved path or NULL (→ fatal "Cannot locate Mission file"). CONFIRM BY: reverse it. |
| `FUN_004bd0e0` | `chunk_file_parse()`   | CONFIRMED. Generic: vfs_lod a chunked file, parse common header table DAT_005061e8 (2 entries) then a CALLER-supplied table. Reusable loader. |
| `FUN_004bd980` | `chunk_file_read_string()` | CONFIRMED. Same load + common header, then 1-entry table DAT_005064d0; copies the handler's stack string out. Pulls one named string field from a chunked file. |
| `FUN_004bccb0` | `chunk_table_parse()` | CONFIRMED. Generic table-driven IFF/tagged-chunk reader (used for .sdf/.vcf/mission — the BWD2/REV family). Args: (data, descriptor_table, table_count, param4, param5(name), mode, end). Data = chunk stream: [u32 tag][u32 byte_len][payload], advance by byte_len; "EXIT"(0x54495845) terminates. Descriptor entry = 0x10 bytes: +0x00 FourCC tag, +0x08 **handler fn ptr**, +0x0c flags (0x04/0x08/0x40 mandatory, 0x80 seen, 0x100/0x200/0x400 count/array, 0x800/0x1000/0x2000 select extra arg = param4/descriptor/param5, bit1 = payload skips 8-byte header). For each matched chunk calls `(*handler)(payload, extra, end)`. **The actual data (incl. placement) is read by the per-entry HANDLERS, not here.** mission_file_load passes tables DAT_005061e8 (2 entries) + DAT_00506208 (7 entries). |
| `FUN_00488d30` | Called very frequently in gameplay loop | No args. Cheap call — yield? input poll? ??? WHY GUESSED: called 3+ times per frame with no apparent side effects on state. CONFIRM BY: reverse it. |
| `FUN_004a2ce0` | Returns float used for timing deltas | No args. Return value compared against stored floats with 1.0 and 10.0 thresholds. Time in seconds? ??? WHY GUESSED: delta comparisons `>= 1.0` and `>= 10.0` suggest seconds, not ms. CONFIRM BY: reverse it or check callers for unit context. |
| `FUN_00431260` | `asset_list_next()` ??? | Args: (out type_char, out name_buf, cursor, end). Manifest iterator used by assets_preload — returns next entry's type char ('p'/'t'/'x'/'v'/'a'/'g') + asset name, advances cursor; returns next cursor (0 = done). CONFIRM BY: reverse to pin the manifest line format. |
| `FUN_00489d40` | D3D texture upload ??? | Args: (loaded texture, type flag 0/1/2). Called only when display_ready() in assets_preload's x/v/a branch. Likely uploads a texture to the D3D/hardware path. CONFIRM BY: reverse it. |
| `FUN_0043ce90` | obj render-state rebuild ??? | Args: (object ptr). Called by obj_model_set_state right after the object's mesh changes. Likely recomputes bounding box / collision / render state from the new mesh. CONFIRM BY: reverse it. |

---

## Object / Geometry Mesh Cache

CONFIRMED by: full decompilation of `obj_system_init` (FUN_0043a6d0),
`geo_cache_acquire` (FUN_0043a770), and `geo_build_mesh` (FUN_0043aa60).

Despite the `g_obj_*` naming, this is a **named, LRU-evicting cache of decoded
GEO meshes** — NOT the live game-entity (vehicle/AI) store. Meshes are decoded
on demand, cached in `g_object_heap`, indexed two ways, and evicted oldest-first
when the heap (~2 MB / 1.3 MB by RAM) runs out of room. `mem_prefetch_objects`
walks it every frame to page the geometry in before it's drawn.

### Two heaps
- `g_object_heap`  (DAT_0052fc40) — the **mesh data buffers** (node+0x0c → here).
- `g_object_heap2` (DAT_0052fc44) — the **cache node structs** (~0x20 bytes).

### Cache node layout (0x20 bytes, heap2 allocation) — fully confirmed
| Offset | Field |
|--------|-------|
| `+0x00` | `char name[8]` — uppercased 8-char name, the by-name key (`+0x04` is just the second half) |
| `+0x08` | `uint32 refcount` — set 1 on create, `++` on acquire; **0 ⇒ idle, sits on the LRU free-list and may be evicted** |
| `+0x0c` | **data ptr** → decoded mesh buffer in g_object_heap |
| `+0x10` | next in by-ptr table chain (`g_obj_buckets`) |
| `+0x14` | next in by-name table chain (`g_obj_buckets2`) |
| `+0x18` | next in LRU list (`g_obj_list2`) |
| `+0x1c` | prev in LRU list |

### Three intrusive structures (each node is in all three)
1. **By-pointer index** `g_obj_buckets` — 2029 heads; bucket = `(data_ptr*0x6cd + 0xaab) % 0x7ed`; chain via +0x10. Used to find a node from its mesh buffer (e.g. on free).
2. **By-name index** `g_obj_buckets2` — 2029 heads; the 8-byte name is read as a 64-bit value, folded `hi ^ lo`, masked `& 0xdfdfdfdf` (ASCII upcase), bucket = `(folded*0x6cd + 0xaab) % 0x7ed`; chain via +0x14. Used to look a mesh up by name on load.
3. **LRU list** `g_obj_list2` (head/oldest) … `DAT_0052fc4c` (tail/newest) — doubly-linked +0x18/+0x1c; eviction takes the head.

Hash constant for both tables: `h(k) = (k * 0x6cd + 0xaab) % 0x7ed`
(= `(k*1741 + 2731) % 2029`). Note: the by-name key is `toupper`'d before
hashing AND the hash re-masks `& 0xdfdfdfdf` — belt-and-suspenders case-fold.

### Acquire — `geo_cache_acquire(name_ptr)` (FUN_0043a770)
The single entry point callers use, and the only caller of geo_build_mesh:
```
upper8 = toupper(name[0..7])
hit = walk g_obj_buckets2[hash_name(upper8)] for a node whose 8-byte name matches
if hit:
    if refcount++ == 0: unlink node from LRU free-list   # was idle, now in use
    return node->mesh
else (miss):
    img = vfs_lod(name)                  # load GEO image via VFS; 0 => return 0 (fail)
    size = vfs_exists(name)
    node = HeapAlloc(g_object_heap2, 0x20)
    node->name = upper8;  node->refcount = 1
    node->mesh = geo_build_mesh(img, size)
    vfs_free(img)
    link node into g_obj_buckets2[hash_name]  (by-name, via +0x14)
    link node into g_obj_buckets[hash_ptr(node->mesh)]  (by-ptr, via +0x10)
    if g_geo_log_file: fprintf("%s %d", name, size)
    return node->mesh
```

### Release/deref — `geo_cache_release` (FUN_0043a9a0) — CONFIRMED
The **release/deref** half. Takes the **mesh data pointer**, looks the node up via
`g_obj_buckets` (by-ptr index — this is exactly why that index exists), does
`refcount--`, and on 0 appends to the LRU tail `g_meshcache_lru_tail`. Confirmed by
use in obj_model_clone (`release(acquire(name))`) and obj_model_set_state (releases
the old mesh by ptr before acquiring a new one). **The mesh cache is now complete:
obj_system_init + geo_cache_acquire + geo_cache_release + geo_build_mesh.**

---

## Object Model Registry (`g_objmodel_registry`)

CONFIRMED by: `obj_model_clone` (FUN_00438a80). Partial — only the clone path seen.

A **per-object render/model record**, separate from the mesh cache. A 2029-bucket
hash table (`g_objmodel_registry` @ 0x00529bf0) keyed by the **game object's
pointer**, mapping each live object to its mesh + model sub-parts. Two heaps:
`g_objmodel_heap` (the 0x3c records) and `g_objmodel_node_heap` (the 0x74 nodes).

### Model record (0x3c bytes, g_objmodel_heap)
| Offset | Field |
|--------|-------|
| `+0x00` | key = **object pointer** (hash key) |
| `+0x04` | currently-bound mesh ptr (from geo_cache_acquire) — also mirrored into object+0x5c |
| `+0x08` | **variant index** — which sub-part node is selected (set by obj_model_set_variant) |
| `+0x0c` | **slot index** — which 0x10-byte name within the node (set by obj_model_set_slot; init -1) |
| `+0x10` | previous slot index (1-deep undo cache; init -1) |
| `+0x14` | currently-bound model **name** (≤15 chars) — the geo_cache_acquire key |
| `+0x24` | previous slot's 16-byte name (undo cache, paired with +0x10) |
| `+0x34` | head of the sub-part node list |
| `+0x38` | hash-chain next |

Sub-part node (0x74 bytes): a table of **7 × 0x10-byte mesh-name slots** (0x70 bytes)
+ next ptr at `+0x70` (singly-linked, NULL-term). So the variants form a 2D grid:
`[node = variant index][slot = record+0x0c]` → a 16-byte mesh name. obj_model_set_state
walks to node `index`, reads slot `record+0x0c`, and swaps the bound mesh if its name
changed (LOD / damage state / part swap).

### Game object struct (the hashed pointer) — fields seen so far
| Offset | Field |
|--------|-------|
| `+0x5c` | cached mesh ptr (set by obj_model_clone) |
| `+0x84`..`+0x93` | 4 dwords (copied as a block) |
| `+0x94`..`+0xab` | 6-dword block — likely a transform/orientation |

### Still needed
All 4 geo_cache_acquire callers (obj_model_clone / _set_variant / _set_slot / and
assets_preload) only ever *consume* an existing object pointer — none allocate one.
So the **game object struct is allocated outside the mesh-acquire call graph.** Next
threads to the real entity store:
- The g_objmodel_registry **create/register** function — i.e. the *other* writer of
  DAT_00529bf0 (besides obj_model_clone), which first binds a fresh object+name.
- **Whatever allocates the object struct** and writes object+0x5c / reads the
  +0x84..+0xab transform block — find via xrefs to those object offsets, or callers
  of obj_model_clone / obj_model_set_variant (they pass the object in).

### GEO/OEG mesh file format — CONFIRMED & DECODED (2026-06-13b)
**The "token stream" theory was wrong — it was LZO compression.** ZFS entries for the
`.geo`/g-tier-`.pak` meshes are stored **LZO1Y-compressed** (entry flags low byte = 0x04,
uncompressed size = `flags >> 8`). The leading `0x20`/`0x22` byte we kept seeing was the
**LZO compression marker**, not part of the asset; the `0x4c/0x4d/0x5c/0x5d` "markers"
were LZO copy tokens. After decompression the buffer is a clean **flat** layout that
`geo_build_mesh` parses directly (verified: NEEDLE 286→592B closes exactly at EOF;
A41ATNK1 4500→17386B → 50 verts/21 faces, bbox 12×2.5×11, geometry is an open
octagonal tub — renders correctly). What these meshes *represent* (object class,
scenery vs. vehicle, the meaning of name tokens like `fnsg`/`plin`) is NOT yet
confirmed and is deliberately left unlabeled.

Decompressed OEG layout (`param_1` points at byte 0 = the magic; **no lead byte**):
```
0x00  u32  magic = 0x2e47454f "OEG."
0x04  u32  count            (purpose TBD; scales loosely with size)
0x08  char name[16]         (null-padded, FIXED 16-byte field — "NEEDLE","A41ATNK1")
0x18  u32  nVerts           (= geo_build_mesh param_1[6])
0x1c  u32  nFaces           (= param_1[7])
0x20  u32  ?                (param_1[8])
0x24  vec3f  posA[nVerts]   (12B each — positions; copied to out+0x18 region)
      vec3f  posB[nVerts]   (12B each — 2nd vertex set, normals? copied to out+...)
      faces[nFaces]:        (start = (nVerts*6+9)*4 from base)
        +0x04 u32 nFaceVerts
        +0x37 entry[nFaceVerts], 0x10B each:
              +0x00 u32 vertex index (into posA)
              +0x04 3×dword (uv / per-vertex data)
        record size = 0x37 + nFaceVerts*0x10
```
A `.pak` holds **multiple** OEG records (the tank pak is 17386B; the first mesh ends at
3831). `vfs_lod_pak` returns `pak_base + PakEntry[+0x14]` to select a specific named
record. So a full vehicle = several OEG records (hull/turret/wheels/etc.) acquired by name.

**TOOL BUG found & to fix:** `tools/zfs_dump get` returned the *compressed* bytes (286
not 592) — it isn't decompressing on the `get` path (works via `zfs_read` though; used
that to extract the real bytes). `vfs_lod_cached` correctly sizes by `vfs_get_uncompressed_size`
and `vfs_read_file` decompresses into the buffer — the engine path was always right.

Cross-loader helper `FUN_004679b0` is a generic name/resolve check (used by both mesh
and audio loaders), NOT texture-specific.

---

## Mission file & object placement (2026-06-13)

CONFIRMED by: mission_file_load, chunk_table_parse, the *DEF/OBJ handler chain, and the
two descriptor tables read from data. This answers "where are world objects positioned".

**Load chain:** scenario name → `mission_file_load` (FUN_004bd1b0) resolves the mission
file (`mission_file_locate` FUN_004bf370, default ext `DAT_00506528`), `vfs_lod`s it, and
runs `chunk_table_parse` (FUN_004bccb0) over it — first with the common header table
`DAT_005061e8` (2 entries), then the **mission body table `DAT_00506208` (7 entries)**.

**`chunk_table_parse`** is a generic, reusable IFF/tagged-chunk reader (also used for
`.sdf`/`.vcf`). Descriptor entry = 0x10 bytes: `+0x00` FourCC tag, `+0x08` handler fn ptr,
`+0x0c` flags (mandatory/seen/count-array/which-extra-arg/skip-8-byte-header). Chunk stream
= `[u32 tag][u32 byte_len][payload]`; `EXIT` (0x54495845) terminates. It dispatches each
chunk to its handler `(payload, name, end)`. The format is **nested** — a handler can run
`chunk_table_parse` again on a sub-table:

```
mission file
└─ DAT_00506208 (7): WDEF TDEF RDEF ODEF LDEF ADEF EXIT   (handlers FUN_004bd450..4f0, thin wrappers)
     └─ ODEF → FUN_004b3630 → DAT_00504928 (3): OREV, OBJ(array, flag 0x400), EXIT
                                   └─ OBJ → FUN_004b3660  (reads ONE placed object)
```

**`FUN_004b3660`** (the per-OBJ reader) is the placement reader and the entry to entity
creation:
1. `FUN_004b3060(payload,end,subtag)` → the object's **geometry/definition name** (fatal
   "Object found with no geometry").
2. Type-1 named markers: name `spawn`/`regen` → `FUN_0044c4a0`; `check` → `FUN_00444ad0`.
3. Object **type code** (`+0x64`) switch → `FUN_004b3e20(...)` allocates the object struct
   (the entity). Richer types parse sub-table `DAT_00504958` (6 entries) for per-object fields.
4. Builds a **48-byte transform** (`local_88`, likely a 3×4 matrix = 12 floats) via
   `FUN_004b3db0`, then **`FUN_00453d50(entity, transform, …)`** spawns it into the world
   (fatal "I'76 Nitro Pack cannot create entity") — **this is the entity-store insert** —
   followed by `FUN_004540b0` to link it.

So each placed object = a geometry/def name + a 3×4 world transform inside an `OBJ` chunk;
`FUN_00453d50` is the live-entity creator (the long-sought entity store entry point).
Spawn points, regen points and checkpoints are `OBJ` records too.

### Renamed/identified function & global addresses (this thread)
| Address | Name | Status |
|---------|------|--------|
| `FUN_004bd1b0` | `mission_file_load` | applied |
| `FUN_004bf370` | `mission_file_locate` | applied |
| `FUN_004bccb0` | `chunk_table_parse` | applied |
| `FUN_004bd0e0` | `chunk_file_parse` | applied |
| `FUN_004bd980` | `chunk_file_read_string` | applied |
| `FUN_0041b3c0` | `sound_buffer_load` | proposed (confirmed body) |
| `FUN_0046d1c0` | `palette_blackout` | proposed (confirmed) |
| `FUN_0049f7e0` | `scenario_classify` | proposed (confirmed) |
| `FUN_004bd4b0` | `mission_odef_handler` | proposed (confirmed wrapper) |
| `FUN_004b3630` | `odef_parse` | proposed (confirmed wrapper) |
| `FUN_004b3660` | `obj_record_read` | proposed (confirmed body) |
| `FUN_004b3e20` | `object_alloc` | proposed |
| `FUN_00453d50` | `entity_spawn` | proposed (strong: "cannot create entity") |
| `DAT_00526a54` | `g_geo_load_depth` | proposed (confirmed) |
| `DAT_005e5ccc`/`DAT_005e5cd0` | `g_mission_buf` / `g_mission_end` | proposed |
| `DAT_005f9b2c`/`DAT_005f9b30` | `g_mission_number` / `g_mission_type` | proposed |
| `DAT_00503d78` | `g_scenario_category` | proposed |
| `DAT_00506208` | `g_mission_chunk_table` | proposed |
| `DAT_005061e8` | `g_chunk_header_table` | proposed |
| `DAT_00504928` | `g_odef_chunk_table` | proposed |
| `DAT_00504958` | `g_obj_subchunk_table` | proposed |
| `DAT_0065465c`/`DAT_00654668` | `g_sound_bytes_total` / `g_sound_heap` | proposed |

Tentative (inferred, bodies not fully seen): `FUN_004bd450/470/490/4d0/4f0/690` =
`mission_{w,t,r,l,a}def_handler`/`_exit`; `FUN_004bd510`/`FUN_004bd190` = OBJ sub
`OREV`/`EXIT` handlers; `FUN_004b3060` = `chunk_field_get`; `FUN_004540b0` =
`entity_register`; `FUN_0044c4a0` = `spawnpoint_register`; `FUN_00444ad0` =
`checkpoint_register`; `FUN_004b3db0` = `transform_build`; `DAT_00503c8c` =
`g_scenario_class_table`; `DAT_00506528` = `g_mission_ext`.

---

## game_session_run — Structure

CONFIRMED by: full decompilation of `game_session_run` (FUN_00430eb0).

`game_session_run(param_1, param_2, param_3, param_4)` is the outer session
coordinator. It does NOT implement the frontend/menu UI — that lives entirely
inside NITSHELL.DLL (`ShellMain`). This function builds the engine↔shell
interface and blocks on `ShellMain` until the user makes a selection.

### Execution flow

```
1. session_frame_a()         — push render/thread state
   display_toggle(1)         — enable display
   set_multiplayer(0)        — reset to singleplayer
2. Build 32-entry callback table on stack (&local_80)
   Each entry is a function ptr or code label — the engine API for the shell
3. GetProcAddress(nitshell, "ShellMain")
   GetProcAddress(nitshell, "ShellWindowProc") → DAT_00526a3c
   Either missing → return 1 (fatal)
4. g_gamestate = 6           — frontend-active state
   DAT_00653fe0 = 0
   (*gfx_init)(&render_surface, 0)   — init graphics mode 0
   pre_shell_setup()
5. ShellMain(nitshell_handle, 0, p2, p3, p1, video_flags,
             &DAT_004f30c8, &DAT_00653b24, &callback_table,
             param_4, saved_gamestate, &DAT_00653b00, &DAT_004fc380)
   → returns uVar5; shell_result (low byte uVar9) = uVar5 & 0xff
6. Decode shell_result:
   0xff → skip data copy, go to step 7
   5   → multiplayer: set_session_mp(1), session_mp_init(param_4),
         copy session descriptor fields from param_4 into globals
   2/4 → singleplayer: memcpy DAT_00653b29 → DAT_00653ff0 (scenario name)
   3   → set_multiplayer(1), then same memcpy as 2/4
7. (*gfx_init)(&render_surface, video_flags)  — reinit graphics with session mode
   session_frame_b()
   session_frame_a()         — re-push state
   display_toggle(0)         — disable display
8. If shell_result != 0xff:
   session_init_a()          ← unknown
   session_init_b()          ← unknown
   session_init_c()          ← unknown
   pix_init()                ← build LOD sprite table
9. return uVar5
```

### ShellMain callback table (32 entries, `&local_80`)

Passed by reference as arg 9 to ShellMain. NITSHELL.DLL uses these to call
back into the engine for rendering, input, audio, etc.

```
[+0x00] &LAB_004b5230     [+0x10] &LAB_004b79f0     [+0x20] FUN_004bb8e0
[+0x04] FUN_004b51a0      [+0x14] FUN_004b5ef0       [+0x24] FUN_004bb810
[+0x08] FUN_004bf150      [+0x18] &LAB_004b77e0      [+0x28] FUN_004198b0
[+0x0c] &LAB_004b6ea0     [+0x1c] &LAB_004b7900      [+0x2c] &LAB_0041a1f0
[+0x30] &LAB_0041a330     [+0x40] FUN_004190f0       [+0x50] FUN_00418e80
[+0x34] &LAB_0041a3f0     [+0x44] &LAB_00468580      [+0x54] FUN_00418f20
[+0x38] FUN_00419070      [+0x48] FUN_0049aec0       [+0x58] &LAB_004bd980
[+0x3c] FUN_00419090      [+0x4c] FUN_004bfe60       [+0x5c] &LAB_0049ac40
[+0x60] &LAB_004a2990     [+0x68] FUN_004b7bf0       [+0x74] &LAB_004be8e0
[+0x64] &LAB_004b7bc0     [+0x6c] FUN_00468340       [+0x78] &LAB_004be830
                          [+0x70] FUN_00467940        [+0x7c] &LAB_004be890
```

All 32 entries are uncharted — reversing ShellMain is the next step to
understand what each slot does.

### Shell result values (`DAT_00653b20`)

| Value | Meaning |
|-------|---------|
| `0xff` | User quit the shell without starting a session |
| `2`   | Singleplayer scenario selected |
| `3`   | Singleplayer + set_multiplayer(1) — possibly LAN/direct IP session |
| `4`   | Singleplayer scenario selected (variant) |
| `5`   | Full multiplayer session — network session descriptor in param_4 |

### Implication for our implementation

We need to write our own `ShellMain` that replaces NITSHELL.DLL's. It receives
the callback table and must return a valid `shell_result`. A minimal stub can
return result=2 with a hardcoded scenario name to get into gameplay. The
callback table entries need to be reversed to understand what NITSHELL calls
back into the engine for.

---

## Session End Flags (`FUN_00448820` return value bitmask)

CONFIRMED by: each bit is checked in an if-chain, and the matching
string literal (e.g. `s_***_%s_has_reached_%d_kills!`) confirms the meaning.

| Bit      | Value  | Meaning |
|----------|--------|---------|
| `0x100`  | 256    | Time limit elapsed |
| `0x200`  | 512    | Points limit reached |
| `0x400`  | 1024   | Kill limit reached |
| `0x800`  | 2048   | Lap limit reached |
| `0x1000` | 4096   | Capture limit reached |

---

## String Table System

CONFIRMED by: named CRT function `StrLookupCreate`, `StrLookupFind`,
`StrLookupDestroy` visible in decompilation. Filename `lang.txt` visible
as a string literal.

- File: `lang.txt`
- API:
  - `StrLookupCreate(filename)` → handle
  - `StrLookupFind(handle, key_string)` → `const char*`
  - `StrLookupDestroy(handle)`

Team name pointer array at `0x004f3098`:
```
PTR_s_Grey_Hounds      004f3098   offset +0x00
PTR_s_Puce_Panthers    004f309c   offset +0x04
PTR_s_Crimson_Kings    004f30a0   offset +0x08
PTR_s_Yellow_Jackets   004f30a4   offset +0x0c
PTR_s_Aqua_Marines     004f30a8   offset +0x10
PTR_s_Mauve_Mayhem     004f30ac   offset +0x14
PTR_s_Mr_Browns_Clowns 004f30b0   offset +0x18
PTR_s_Men_In_Black     004f30b4   offset +0x1c
PTR_s_Red_Dawn         004f30b8   offset +0x20  ← Team B base (iVar8 offset into here)
PTR_s_Gang_Green       004f30bc   offset +0x24
PTR_s_Purple_Reign     004f30c0   offset +0x28
PTR_s_Black_Plague     004f30c4   offset +0x2c
```

Team A / Team B split:
- Team A: `(&PTR_s_Grey_Hounds_004f3098)[team_index]`
- Team B: `(&PTR_s_Red_Dawn_004f30b8)[team_index]`
- CONFIRMED by: both pointer bases used in sprintf for win/lose message.

???  Whether teams A and B map to specific multiplayer team slots is
     unknown. WHY GUESSED: the two bases (Grey Hounds / Red Dawn) split
     the 12 teams into two groups of 6 by position. CONFIRM BY: find
     where team assignments are written and read during a network session.

---

## ZFS Archive Format

CONFIRMED by: reversing `zfs_create`, `zfs_load_dir`, `zfs_find`.

### File Layout

```
[  0] char[4]   magic = "ZFSF"
[  4] uint32    version = 1
[  8] uint32    0x10  (unknown — always this value)
[ 12] uint32    entries_per_block = 100
[ 16] uint32    total_file_count  (includes deleted entries)
[ 20] uint32    creation_flags    (passed as param_2 to zfs_create; purpose unknown)
[ 24] uint32    first_block_offset (= 28 for fresh archives, read from header — NOT hardcoded)
```

Then blocks, starting at `first_block_offset`. Each block is exactly **3604 bytes (0xe14)**:
```
[ +0] uint32    next_block_offset  (absolute file offset; 0 = this is the last block)
[ +4] entry[100]  directory entries, 36 bytes (0x24) each
```

Blocks form a **singly-linked list** via `next_block_offset`. `zfs_load_dir` walks the chain.

### Directory Entry (36 bytes)

```c
typedef struct {
    char     name[16];    // +0x00  null-padded, DOS 8.3, all-caps on disk
    uint32_t offset;      // +0x10  absolute byte offset of file data within .zfs
    uint32_t index;       // +0x14  sequential 0-based index
    uint32_t size;        // +0x18  stored (possibly compressed) size in bytes
    uint32_t timestamp;   // +0x1c
    uint32_t flags;       // +0x20  see flags below
} ZFSEntry;
```

### Entry Flags

```
bit 0      = deleted         — filtered out by zfs_load_dir; never appear in live entry array
bit 1      = compression A   } at least one is set on every compressed file; meaning of each
bit 2      = compression B   }   distinct value TBD (LZO vs other algo? level?)
bits 8–31  = uncompressed size — the size after decompression; buffer size caller must allocate
```

CONFIRMED by: observing real archive — nearly every entry has bit 1 or 2 set, and
`flags >> 8` is consistently 1.5–2× the stored size (typical compression ratio for
audio/texture data). This also explains `vfs_get_uncompressed_size`: it reads this field.

`zfs_find` return value therefore means:
- `-1`           → not found
- `flags >> 8`   → file is compressed; value = bytes needed for decompressed output buffer
- `entry.size`   → file is uncompressed; stored size = output size

??? Whether bit 1 vs bit 2 encodes different compression algorithms or parameters.
    CONFIRM BY: find two files where only one bit differs and compare their data headers.

### ZFS Handle Struct (44 bytes, heap-allocated)

Returned by `zfs_load_dir`; passed to all zfs_* functions as `param_1`.

```c
typedef struct {             /* index as uint32_t* */
    FILE     *file;          /* +0x00  [0]  open file handle */
    char      path[16];      /* +0x04  [1]  archive path (null-terminated; max 15 chars — DOS basename only) */
    int       num_blocks;    /* +0x14  [5]  = ceil(total_count / 100) */
    int       active_count;  /* +0x18  [6]  live (non-deleted) entries; used as bsearch count */
    uint32_t  xor_key;       /* +0x1c  [7]  per-archive XOR encryption key (0 = no encryption).
                                             Stored in header bytes 20–23. After read/decompress,
                                             every uint32 of output is XOR'd with this value. */
    int       total_count;   /* +0x20  [8]  raw count from header (including deleted) */
    uint32_t *block_offsets; /* +0x24  [9]  heap array[num_blocks] of each block's file offset */
    ZFSEntry *entries;       /* +0x28  [10] heap array[active_count], sorted by name for bsearch */
} ZFSHandle;
```

### Open Modes

```
zfs_open(path, 0)   → read-only  ("rb"),   unbuffered (_IONBF)
zfs_open(path, 1)   → read-write ("r+b"),  creates archive via zfs_create if ENOENT
```

### Compression

CONFIRMED by `zfs_decompress_into_impl`:
- `ZFSEntry.size` = **compressed** (stored) size on disk
- `ZFSEntry.flags >> 8` = **decompressed** size (output buffer size)
- Algorithm: **LZO** — confirmed by `lzo_decompress_dispatch` and `__lzo_init_v2` call
- LZO version `0x1000` (1.0x era, matches 1997 release date)
- Two variants selected by entry flags low byte:
  - `flags & 2` → `FUN_004438f0` — **lzo1x_decompress** (tentative)
  - `flags & 4` → `FUN_00443c88` — **lzo1y_decompress** (tentative)
  - Neither bit set → error (should not occur on valid entries)
- Decompressor ABI: `(src, src_len, dst, &dst_len, wrkmem=NULL)` — standard LZO interface
- `g_lzo_initialized` (`DAT_005e4ff4`) — lazy-init flag; library initialized on first decompress call

### XOR Encryption

CONFIRMED by `zfs_decompress_into_impl`:
- `ZFSHandle.xor_key` (header bytes 20–23) is a per-archive 32-bit XOR key
- After decompression (or raw read for uncompressed files), every `uint32_t` of output
  is XOR'd with this value: `for (i=0; i < size/4; i++) output[i] ^= xor_key`
- `xor_key == 0` = no encryption (nitro.zfs is almost certainly 0)
- Key is set at archive creation (`zfs_create` param_2) and never changes

---

## VFS Layer

Sits above the ZFS layer. Maps filenames to sources (loose directories or ZFS archives)
and handles decompression, caching, and LOD selection.

### VFS File Table (`g_vfs_file_table`)

Global sorted array. `DAT_005e5ee0` = pointer, `DAT_005e5ee4` = count. Stride 0x30 = 48 bytes.
Binary-searched by filename (first 16 bytes) in `vfs_lookup`.

```c
struct VFSFileEntry {       /* 48 bytes, sorted by name for bsearch */
    char     name[16];      /* +0x00  lowercase, DOS 8.3 */
    uint32_t sources[8];    /* +0x10  source bitmask — bit N = this file exists in source N
                                       32 bytes = 256 bits = up to 256 sources */
};
```

### VFS Source Table (`g_vfs_source_names` / `g_vfs_source_types`)

Not two separate arrays — both are the same struct array; `g_vfs_source_types` points to the
`type` field of the first entry. Stride 0x10c = 268 bytes. `g_vfs_source_count` entries.

`vfs_lookup` returns a pointer into this table (the best available source for the requested file).

```c
struct VFSSource {          /* 268 bytes, indexed 0..g_vfs_source_count-1 */
    char     path[256];     /* +0x000  source path string:
                                         loose dir → directory name (e.g. "addon")
                                         ZFS       → archive filename (e.g. "nitro.zfs") */
    int      type;          /* +0x100  0=loose directory, 1=ZFS archive */
    int      base_path_idx; /* +0x104  index into DAT_005e6120 root-path table (stride 0x300) */
    int      zfs_handle;    /* +0x108  cached open ZFSHandle* (0 = not yet opened; lazy-init) */
};
```

CONFIRMED by: vfs_lookup return expression `&g_vfs_source_names + index * 0x10c`;
vfs_read_file_impl field accesses at +0x100, +0x104, +0x108.

Source selection in `vfs_lookup`: prefers loose-dir sources over ZFS when `DAT_005f9b24`
(drive-override mode) is active; otherwise returns first source that has the file.

### File Existence / Size Cache

The VFS file cache lives in a private Win32 heap (`g_cache_heap`) created per-session
by `vfs_cache_init`. Capacity is RAM-adaptive (6–11MB). Max 2000 entries (`g_file_cache_max`).
Torn down and rebuilt each session via `vfs_cache_destroy` + `vfs_cache_init`.

`vfs_exists` returns the **uncompressed file size** (not a bool), 0 = not found.
Two-level lookup before falling back to `vfs_get_uncompressed_size`:

1. (TBD) — fast first-level cache lookup (previously misidentified as `pak_find_entry`; that is unrelated to VFS size cache)
2. Hash table `g_file_cache_buckets`: 2009 (0x7d9) buckets, each a linked-list head

Hash function (CONFIRMED from pak_unload / vfs_cache_deref):
```c
h = 0;
for each char c in name:
    h = h * 2 ^ c;
bucket = abs(h) % 2009;
```
Case-insensitive lookup via `_stricmp`.

Cache entry layout (CONFIRMED from pak_unload):
```
[0x00..0x0f]  filename[16]      null-terminated, case-insensitive key
[0x10]        size (int)        uncompressed size, returned by vfs_exists
[0x14]        refcount (short)  incremented on load, decremented by vfs_cache_deref
[0x16]        (unknown, 2 bytes)
[0x18]        bucket_next (ptr) next entry in same hash bucket
[0x1c]        lru_prev (ptr)    LRU list: toward g_lru_head (newer); NULL = most recent
[0x20]        lru_next (ptr)    LRU list: toward g_lru_tail (older)
```
Total entry size: exactly **0x24 header + `size` bytes of file data appended**.
Allocated as one `HeapAlloc(g_cache_heap, size + 0x24)`. `vfs_lod_cached` (the cache
acquire) returns `entry + 0x24` — a pointer to the raw file bytes. Allocation is
capped: `size + 0x24 > 0x7fff7` (~512 KB) is fatal ("Cannot allocate space for %s").

When refcount hits 0, entry is appended to LRU free list (`g_lru_head` / `g_lru_tail`)
but NOT immediately freed — stays available until the eviction path needs space.
`vfs_lod_cached` is the acquire (refcount++ / un-LRU); `vfs_cache_deref` is the release.

3. On total miss: `vfs_get_uncompressed_size(name)` — reads from ZFS entry flags

### VFS Source Table

Each entry is 0x10c = 268 bytes. Global arrays `g_vfs_source_names` / `g_vfs_source_types`
use the same stride (fields within the same struct, not separate parallel arrays).

Source types:
- `0` = loose directory — enumerated via `FindFirstFile`
- `1` = ZFS archive — `"nitro.zfs"` is explicitly checked and skipped in `vfs_enumerate_sources`;
         its contents are enumerated via a different path (ZFS directory walk)

### `vfs_read_file_impl` Calling Convention

```c
// buf == NULL  → allocate, fill, and return pointer (caller must vfs_free())
// buf != NULL  → fill buf; return buf on success, NULL if capacity < actual size
void *vfs_read_file_impl(const char *name, size_t *size_out, void *buf, int capacity);
```

Path resolution:
- `DAT_005f9b24 == 0` (normal): `base_path_table[source_index] + "\\" + filename`
- `DAT_005f9b24 != 0` (drive override): `get_drive_letter() + ":\\" + filename`
- If path contains `".zfs"` → ZFS read path; else → fopen/fread loose file

### Other Named VFS Functions

| Name | Notes |
|------|-------|
| `get_drive_letter()` `FUN_00468340` | No args (or source name). Returns char — a DOS drive letter. Used for loose-file path building. |
| `str_tolower_inplace()` | In-place ASCII lowercase. Called on filenames after strncpy in vfs_enumerate_sources. |

---

## NITSHELL.DLL — Renderer Abstraction

CONFIRMED: loaded via `LoadLibraryA("NITSHELL.DLL")`. Handle stored in
`DAT_00653f78`. Function pointers populated after load (exact mechanism
not yet seen — probably GetProcAddress calls in an init function).

Function pointers stored in globals:

| Global         | Address      | ??? Role | WHY GUESSED |
|----------------|--------------|----------|-------------|
| `DAT_00653f3c` | `0x00653f3c` | ??? `BeginFrame(surface*)` | Called first in every "render block". |
| `DAT_00653f40` | `0x00653f40` | ??? `EndFrame(surface*)`   | Called immediately after FUN_0046ecd0. |
| `DAT_00653f48` | `0x00653f48` | ??? `SetPalette(surface*, palette*)` | Takes surface + palette struct. Called after pcx_decode. |
| `DAT_00653f4c` | `0x00653f4c` | ??? `Init(surface*, mode)` | Called with (surface, 0) at startup and mode changes. Returns 0 on fail → fatal_error. |
| `DAT_00653f60` | `0x00653f60` | ??? `Flip(surface*)`       | Called last in every render block. "Present" equivalent. |

CONFIRM ALL OF THE ABOVE BY: reversing NITSHELL.DLL and reading its
export table. That will give real function names and confirm or deny
every guess above.

Observed render block call pattern:
```c
(*DAT_00653f3c)(&DAT_00653b40);            // ???BeginFrame
FUN_0046ecd0(palette_count, palette_ptr);  // upload palette — unknown function
(*DAT_00653f40)(&DAT_00653b40);            // ???EndFrame
(*DAT_00653f60)(&DAT_00653b40);            // ???Flip
```

---

## Render Surface Struct (`DAT_00653b40`)

Size: `0x10b dwords = 1068 bytes`
Source: copied from `FUN_00484960(buffer, hInstance, class_name)` result.

Known offsets (partial — derived from global address arithmetic):
```
+0x00  ...unknown start...
+0xe4  DAT_00653c24  — checked after Init(); nonzero → fatal_error ???
                       WHY GUESSED: compared to 0, on fail calls fatal_error
                       with "Init Graphic Sys" message.
                       CONFIRM BY: reverse FUN_00484960.
+0xe8  DAT_00653c28  — passed to SetPalette ??? and pcx_decode as dest.
                       WHY GUESSED: used in both palette and blit calls.
                       CONFIRM BY: reverse FUN_00484960.
+0xbc  DAT_00653bfc  — passed to video_play as HWND ???.
                       WHY GUESSED: used in DestroyWindow call and video player.
                       CONFIRM BY: reverse FUN_00484960.
```

TODO: fully map by reversing FUN_00484960.

---

## Font System

CONFIRMED by: literal font filenames in decompilation, DAT_005fb0e0 comparison,
and direct format analysis of extracted .fnt files.

```
DAT_005fb0e0 == 6  →  base6x74.fnt,  font_id = 1
DAT_005fb0e0 == 7  →  base6x7.fnt,   font_id = 0
else               →  base6x76.fnt,  font_id = 2
```

??? What DAT_005fb0e0 actually represents (video mode? color depth?
    display resolution?). WHY GUESSED: named "video mode byte" because
    it's also masked `& 0xff` and stored into DAT_00653fe0 before
    Init() calls. CONFIRM BY: find what writes DAT_005fb0e0.

### .fnt File Format

CONFIRMED by direct inspection of all three font files.

```
[0x00]  uint32  magic       = 0x00002e31
[0x04]  uint32  glyph_count = 128  (ASCII 0–127)
[0x08]  uint32  height      — glyph height in pixels (7, 7, or 14)
[0x0c]  uint32  transparent — pixel value meaning "skip" (always 0xff)
[0x10]  uint32  offsets[glyph_count] — absolute file offsets to each glyph

Each glyph (at offsets[i]):
    uint32  width           — glyph advance width in pixels
    uint8   pixels[height × width]
              0xff = transparent (skip)
              0x0e = foreground pixel (draw in caller's chosen colour)
```

Pixel encoding is monochrome — exactly two values used across all glyphs:
`0xff` (transparent) and `0x0e` (foreground). The renderer substitutes the
foreground pixel with whatever palette index is requested at draw time.

Non-printable glyphs (ASCII 0–31) have width=0 and no pixel data.
Space (ASCII 32) has a non-zero width with all-transparent pixels (advance
only, no ink).

Files and dimensions:

| File          | height | Notes                         |
|---------------|--------|-------------------------------|
| base6x7.fnt   |  7     | Standard UI font, 4197 bytes  |
| base6x74.fnt  | 14     | Double-height, 10168 bytes    |
| base6x76.fnt  |  7     | Alternate variant, 10168 bytes|

Implementation: `src/engine/font.c`

---

## Loading Screens

CONFIRMED by: literal filenames in decompilation.

- `addon\loadgame.pcx` — shown during initial boot
- `addon\loadscr.pcx`  — shown during session/mission load
- Custom PCX: if `DAT_00526758[0] == 'p'`, builds path from scenario
  name with `.pcx` extension (strchr('.') to replace or append extension)

---

## Video Files (Smacker format, .smk)

CONFIRMED by: literal filenames in decompilation.

| File / Pointer  | When played |
|-----------------|-------------|
| `introf01.smk`  | Startup, before main loop |
| `credf01.smk`   | After intro |
| `DAT_00653f80`  | ??? Outro — after mission, before returning to frontend. WHY GUESSED: played in the GS_FRONTEND transition. CONFIRM BY: find what string is stored at 0x00653f80. |
| `DAT_00653f90`  | ??? Frontend intro — when returning to menu. WHY GUESSED: played at top of GS_FRONTEND state, different from outro. CONFIRM BY: find what string is stored at 0x00653f90. |

Decoder: libsmacker — confirmed working (integrated in video.c).

---

## PIX / PAK — LOD Sprite System

CONFIRMED by: full decompilation of `pix_init` (FUN_00467cd0) and direct
inspection of extracted files. 3516 PAK+PIX pairs in nitro.zfs.

### Overview

When a 3D object is far from camera the engine substitutes pre-rendered
sprites instead of drawing full geometry. Each object/LOD level has a pair:

| Extension | Role |
|-----------|------|
| `.pix`    | Text index: maps GEO mesh names → record offset in the paired PAK file |
| `.pak`    | Binary: packed records — g-tier = OEG 3D meshes, 16-tier = parametric LOD blobs (NOT sprite pixels; see PAK File Format) |

Files are always paired by base name (e.g. `a1flag1g.pix` + `a1flag1g.pak`).

### Naming Convention

`<object_id><lod_suffix>` where `lod_suffix` is one of:

| Suffix | Meaning |
|--------|---------|
| `g`    | Game quality — highest detail sprites |
| `m`    | Medium LOD |
| `16`   | Lowest LOD — 16-pixel sprites for maximum distance |

Examples: `a4tank1g`, `a4tank1m`, `a4tank16` — three LOD tiers for the same
tank object. `a4` is the object family, `tank1` is the variant.

### PIX File Format (text, CRLF)

```
<entry_count>\r\n
<geo_name> <start_frame> <frame_count>\r\n
... repeated entry_count times
```

Example (`a2fnsg16.pix`, 24 bytes):
```
1
A2_1SG_1.M16 0 1038
```
→ 1 entry: GEO mesh `A2_1SG_1.M16`, sprites start at frame 0, 1038 total
  frames (one per viewing angle, ~0.35° per step).

??? Some .pix files start with `&<count>` instead of a plain integer, and
    contain binary-packed entries after the first line (observed in `g`-tier
    files). WHY GUESSED: `a1flag1g.pix` has `&6\r\n` then binary after the
    first text entry; `a2fnsg16.pix` is pure text. CONFIRM BY: find the
    format string at DAT_004fac88 in pix_init (likely `"&%d"` vs `"%d"`), and
    check whether `vfs_open_text` decodes the binary portion.

### PakEntry Struct (28 bytes = 0x1c)

Built by `pix_init`, sorted by `geo_name` for binary search via `pak_find_entry`.

```c
struct PakEntry {           /* stride 0x1c */
    char     geo_name[16];  /* [0x00] lowercase GEO filename — lookup key */
    uint32_t pix_file_idx;  /* [0x10] index into g_pix_file_names (which .pak) */
    uint32_t start_frame;   /* [0x14] first sprite frame offset in the PAK */
    uint32_t frame_count;   /* [0x18] number of sprite frames */
};
```

`pix_file_idx` is used to reconstruct the `.pak` filename:
`g_pix_file_names[pix_file_idx]` already has the `.pak` extension (pix_init
replaces `.pix` → `.pak` when building that array).

`g_pix_file_names` record layout (stride 0x18, CONFIRMED from vfs_lod_pak):
```c
struct PixFile {            /* stride 0x18 */
    char  pak_name[16];     /* [0x00] .pak filename — passed to vfs_lod_cached */
    void *loaded;           /* [0x10] cached loaded-PAK base ptr (0 = not loaded yet) */
    int   refcount;         /* [0x14] bumped each vfs_lod_pak hit on this pak */
};
```
So `vfs_lod_pak` returns `g_pix_file_names[PakEntry.pix_file_idx].loaded + PakEntry.start_frame`.
NOTE: `start_frame` (+0x14) is added directly to a byte pointer, so for the records
geo_build_mesh consumes it behaves as a **byte offset** into the loaded PAK — its
relationship to the `.pix` text "start_frame" value still needs an empirical check
against real PAK bytes (the leading 0x22-before-OEG byte means offset 0 ≠ the 'O').

### PAK File Format (binary) — partially reversed by inspection (2026-06-13)

CORRECTION: PAKs do NOT contain "pre-rendered sprite pixel data". Confirmed by
extracting real pairs:

- **g-tier** (e.g. `a1flag1g.pak`, 656 B): a record per PIX entry; each record is
  `[1-byte header 0x22]` + an **`OEG.` 3D mesh** (`2e47454f`) + null-terminated GEO
  name (`A11_BAS1`) + mesh body. This is the geometry `geo_build_mesh` decodes.
- **16-tier** (e.g. `a2fnsg16.pak`, 35 B for a 1038-"frame" entry): a tiny
  **parametric far-LOD descriptor** — `[0x15]` header then ~30 bytes of params. NOT
  bitmaps; far too small to be sprite frames. Leading byte differs from g-tier.

So the LOD tiers are: **g = full 3D mesh, m = reduced mesh, 16 = a compact
parametric billboard/impostor**. There is no directly-blittable 2D sprite asset.

UNRESOLVED off-by-one: the g-tier record's `OEG.` magic sits at byte **+1** (after
the 0x22), but `geo_build_mesh` checks the magic at `*param_1`. So either PakEntry's
offset (+0x14) is 1-based past the header, or there's a +1 between vfs_lod_pak's
return and geo_build_mesh that the decompile flattened. CONFIRM BY: dump the actual
runtime pointer, or reverse pix_init's PakEntry offset computation.

---

## ZIX Index File (`nitro.zix`)

CONFIRMED by: inspecting file directly.

Plain text, CRLF line endings. Parsed at startup to build `g_vfs_sources` and `g_vfs_file_table`.

```
<total_file_count>\r\n          e.g. "006704"
<src_type> <src_path>\r\n       e.g. "0 \install\data i76nitro"  (one line per source)
---...---\r\n                   separator
DIR: <archive_name>\r\n         e.g. "DIR: nitro.zfs"   (ZFS source declaration)
---...---\r\n
<src_index> <filename>\r\n      e.g. "1 01cor01.wav"    (one line per file)
...
```

Source type prefix: `0` = loose directory, `1` = ZFS archive.
All 6704 Nitro Pack files are listed as source `1` (nitro.zfs). The loose-directory
source (`0 \install\data i76nitro`) exists for any loose overrides.

`DAT_00590a64` is NOT a direct load of this file. It is a binary sorted table
(stride 0x1c, built at runtime from ZFS directory entries):
```
+0x00  name[16]           — filename
+0x10  offset[4]          — ZFSEntry.offset  (??? confirm)
+0x14  index[4]           — ZFSEntry.index   (??? confirm)
+0x18  uncompressed_size  — flags>>8 for compressed, entry.size otherwise
```
WHY GUESSED: stride 0x1c = 16+4+4+4; +0x18 return value matches
vfs_get_uncompressed_size semantics; 6704 entries × 28 bytes = 183KB fits in RAM.
CONFIRM BY: find the function that writes DAT_00590a64.

---

## Command Line Flags

Parsed by `cmdline(cmdline, &DAT_00526758)`.

CONFIRMED flag effects (seen directly in Fun_Real_Entry):
- Result written into `DAT_00526758` (scenario name buffer)
- `DAT_00526a64 |= 2` — set if some flag present → ORed into session config
- `DAT_00526a68 |= 4` — set if some flag present → ORed into session config

??? Which actual command line strings (-mp, -demo, etc.) map to which
    flags. WHY GUESSED: we only see the effects, not the parsing.
    CONFIRM BY: reverse cmdline.

Scenario name prefix meanings (CONFIRMED by `_strnicmp` calls):
```
"mp*"  → multiplayer  → DAT_00653f74 = 1
"ms*"  → multiplayer  → DAT_00653f74 = 1
"sp*"  → singleplayer → DAT_00653f74 = 0
```

??? What DAT_00653f74 = 1 actually does downstream. WHY GUESSED:
    named "is_multiplayer_scenario" by pattern. CONFIRM BY: find readers
    of DAT_00653f74.

---

## Process Priority

CONFIRMED behavior (but NOT ported — irrelevant on modern hardware):
```c
SetPriorityClass(GetCurrentProcess(), 0x100); // HIGH_PRIORITY_CLASS
// ... FUN_00430860(&DAT_004f36c8) — AI tick ???
SetPriorityClass(GetCurrentProcess(), 0x20);  // NORMAL_PRIORITY_CLASS
```

??? Whether FUN_00430860 is the AI tick. WHY GUESSED: it runs between
    the two priority calls, and priority is bumped specifically for it.
    CONFIRM BY: reverse FUN_00430860.

---

## Implementation Status

### ZFS / VFS Layer

| Component | File | Status |
|-----------|------|--------|
| ZFS archive reader | `src/engine/zfs.c` / `zfs.h` | DONE — open, bsearch lookup, lzo1x+lzo1y decompress, XOR decrypt |
| VFS init (.zix parser) | `src/engine/vfs.c` / `vfs.h` | DONE — parses nitro.zix, builds sorted file table, lazy-opens ZFS handles |
| `vfs_read_file` | `src/engine/vfs.c` | DONE — ZFS and loose-file paths |
| `vfs_exists` | `src/engine/vfs.c` | DONE — returns uncompressed size or 0 |
| `vfs_free` | `src/engine/vfs.c` | DONE |
| VFS file cache | — | NOT YET — no hash-table size cache (vfs_exists hits ZFS every call) |
| `vfs_lod` / `vfs_lod_cached` / `vfs_lod_pak` | — | NOT YET — stubs needed when textures are loaded |
| `vfs_cache_evict` | — | NOT YET |

LZO: uses system `liblzo2` (pkg-config `lzo2`). lzo1x = `flags & 0x02`, lzo1y = `flags & 0x04`.

### Font / String Table

| Component | File | Status |
|-----------|------|--------|
| `.fnt` loader + renderer | `src/engine/font.c` | DONE — `font_load`, `font_free`, `font_draw`, `font_text_width` |
| StrLookup API | `src/engine/strlookup.c` | STUB — returns NULL; GOG has no lang.txt |

### PCX

| Component | File | Status |
|-----------|------|--------|
| PCX loader | `src/engine/pcx.c` | DONE — reads via `vfs_read_file()`, RLE decode, palette extract |

### Startup

| Component | File | Status |
|-----------|------|--------|
| `vfs_init()` wired | `src/main.c` | DONE |
| Loading screen render | `src/main.c` | DONE — `addon/loadgame.pcx` |
| Smacker intro/credits | `src/main.c` | DONE — `smk/introf01.smk`, `smk/credf01.smk` |
| Font smoke test | `src/main.c` | TEMPORARY — draws "INTERSTATE '76" banner; should become proper startup banner |

**Next step:** reverse the 32-entry NITSHELL callback table passed to ShellMain so we can write our own ShellMain stub and get into the gameplay state.

---

## Build & Run

```bash
# Install dependencies (Ubuntu/Debian)
sudo apt install build-essential cmake libsdl2-dev clangd gdb

# Open in VS Code OSS / VSCodium
code .         # or: codium .

# Install recommended extensions when prompted (.vscode/extensions.json)

# Build: Ctrl+Shift+B  (runs the "Build" task)
# Run:   Ctrl+Shift+P → "Tasks: Run Test Task"  (runs the "Run" task)

# Or manually:
mkdir build && cd build && cmake .. && make
cd .. && ./i76
```
