// i76 — reapply Ghidra renames from the project ledger (ghidra_renames.md + REVERSING.md).
// Recovers function/global names after a DB symbol loss. Idempotent; safe to re-run.
// Java GhidraScript: works without PyGhidra/Jython.
//
// RUN: open nitro.exe (auto-analysis done), Window > Script Manager > refresh,
//      select ReapplyI76Renames.java (category i76) > Run. Watch the console.
//@category i76
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

public class ReapplyI76Renames extends GhidraScript {

    private static final String[][] RENAMES = {
        {"0041b3c0", "sound_buffer_load"},
        {"004312f0", "assets_preload"},
        {"00438a80", "obj_model_clone"},
        {"00438d00", "obj_model_set_variant"},
        {"00438e10", "obj_model_set_slot"},
        {"0043a6d0", "obj_system_init"},
        {"0043a770", "geo_cache_acquire"},
        {"0043a9a0", "geo_cache_release"},
        {"0043aa60", "geo_build_mesh"},
        {"0043d070", "face_texture_bind"},
        {"0044def0", "render_scene_solo"},
        {"0044e220", "bsp_insert"},
        {"0044e280", "render_subobject"},
        {"0044e700", "render_attach_list"},
        {"0044e7a0", "render_scene_list"},
        {"0044ebd0", "render_scene_full"},
        {"0044f110", "subpart_emit"},
        {"0044f210", "subpart_emit_attach"},
        {"00453d50", "entity_spawn"},
        {"004679b0", "texture_resolve"},
        {"0046d1c0", "palette_blackout"},
        {"0046f6f0", "light_register"},
        {"0046fca0", "obj_render_prep"},
        {"00471160", "raster_poly"},
        {"004878a0", "raster_poly_cam1"},
        {"00498470", "render_dispatch_ab"},
        {"004984b0", "render_dispatch"},
        {"00498520", "obj_model_draw"},
        {"0049c0b0", "mesh_transform_verts"},
        {"0049c130", "mat4_mul"},
        {"0049c200", "mat4_transform_vec4"},
        {"0049f7e0", "scenario_classify"},
        {"004b0070", "livery_decals_build"},
        {"004b3630", "odef_parse"},
        {"004b3660", "obj_record_read"},
        {"004b3b60", "SDFC"},
        {"004b3be0", "SGEO"},
        {"004b3c40", "SCHK"},
        {"004b3db0", "transform_build"},
        {"004b3e20", "object_alloc"},
        {"004b4060", "SOBJ"},
        {"004b4ab0", "LOBJ"},
        {"004bccb0", "chunk_table_parse"},
        {"004bd0e0", "chunk_file_parse"},
        {"004bd1b0", "mission_file_load"},
        {"004bd4b0", "mission_odef_handler"},
        {"004bd980", "chunk_file_read_string"},
        {"004be990", "vfs_index_build"},
        {"004bf370", "mission_file_locate"},
        {"004bfb10", "vfs_file_exists"},
        {"004f30cc", "g_gamestate"},
        {"004f72c0", "g_keymap"},
        {"004f7e48", "g_render_facemask_tbl"},
        {"00503d78", "g_scenario_category"},
        {"00503e94", "g_input_mode"},
        {"00504928", "g_odef_chunk_table"},
        {"00504958", "g_obj_subchunk_table"},
        {"005061e8", "g_chunk_header_table"},
        {"00506208", "g_mission_chunk_table"},
        {"00526a3c", "shell_window_proc"},
        {"00526a44", "g_exit_code"},
        {"00526a50", "g_geo_log_file"},
        {"00526a54", "g_geo_load_depth"},
        {"00526a6c", "g_window_active"},
        {"00529bf0", "g_objmodel_registry"},
        {"0052bba4", "g_objmodel_heap"},
        {"0052bba8", "g_objmodel_node_heap"},
        {"0052bcd0", "g_obj_buckets"},
        {"0052dc88", "g_obj_buckets2"},
        {"0052fc3c", "g_has_current_tex"},
        {"0052fc40", "g_object_heap"},
        {"0052fc44", "g_object_heap2"},
        {"0052fc48", "g_obj_list2"},
        {"0052fc4c", "g_obj_list2_tail"},
        {"0052fc50", "g_current_tex_name"},
        {"0052fc6c", "g_arr_list2"},
        {"0052fc70", "g_arr_buckets"},
        {"00545ce8", "g_render_vbuf"},
        {"00545cec", "g_render_vcursor"},
        {"00545cf8", "g_render_camera"},
        {"00545d0c", "g_render_ctx_flags"},
        {"00545d10", "g_render_lod"},
        {"00545d58", "g_render_bsp_head"},
        {"00590a64", "g_pak_entries"},
        {"00590a68", "g_pix_file_names"},
        {"00590a6c", "g_pix_file_count"},
        {"00590a70", "g_pak_entry_count"},
        {"0059137c", "g_light_count"},
        {"005913fc", "g_light_table"},
        {"005e5ccc", "g_mission_buf"},
        {"005e5cd0", "g_mission_end"},
        {"005e5ee0", "g_vfs_file_index"},
        {"005e5ee4", "g_vfs_file_count"},
        {"005f9b2c", "g_mission_number"},
        {"005f9b30", "g_mission_type"},
        {"006520a0", "g_cache_heap_size"},
        {"006520cc", "g_lru_tail"},
        {"006520d0", "g_lru_head"},
        {"006520d4", "g_cache_heap"},
        {"00652784", "g_object_heap_size"},
        {"00653b20", "shell_result"},
        {"00653fa0", "g_xform_vbuf"},
        {"00653fa4", "g_xform_vbuf_cur"},
        {"00654000", "g_model_matrix"},
        {"0065465c", "g_sound_bytes_total"},
        {"00654668", "g_sound_heap"},
    };

    @Override
    public void run() throws Exception {
        int fn = 0, lbl = 0, fail = 0;
        for (String[] r : RENAMES) {
            Address addr = toAddr(r[0]);
            try {
                Function f = getFunctionAt(addr);
                if (f != null) {
                    f.setName(r[1], SourceType.USER_DEFINED);
                    fn++;
                } else {
                    createLabel(addr, r[1], true, SourceType.USER_DEFINED);
                    lbl++;
                }
            } catch (Exception e) {
                println("  FAILED " + r[0] + " -> " + r[1] + " : " + e.getMessage());
                fail++;
            }
        }
        println("i76 reapply done: " + fn + " functions renamed, " + lbl
                + " labels created, " + fail + " failures (of " + RENAMES.length + ")");
    }
}
