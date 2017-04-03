#ifndef AC_SHADER_INFO_H
#define AC_SHADER_INFO_H
/* a NIR pass to gather all the info needed to optimise the alloction patterns for the RADV user sgprs */
/* also can be used to get other info we need to optimise in other areas */
/*
 * SCRATCH_RING_OFFSETS - always enabled for all stages with later LLVM. SGPRs 0/1.
 * DESCRIPTOR SETS - needed if shader access any of the sets.
 * PUSH_CONSTANTS - needed if we have push constants or dynamic offsets - we should allow for inlining some
 *                  push constants if there are left over user sgprs.
 * VS:
 * VERTEX_BUFFERS - needed if the vertex shader fetches from an input - we can read this from nir inputs_read.
 * BASE_VERTEX_START_INSTANCE - needs if vertex shader uses one of those inputs
 *
 * PS:
 * SAMPLE_POS - if the shader accessess any sample positions
 *
 * CS:
 * GRID_SIZE: needed if CS acceses grid size
 *
 * GS - GSVS_STRIDE
 *      GSVS_NUM_ENTRIES
 *
 * TCS/TES/LS: layouts required for tess ring access.
 */

struct ac_shader_info {
	uint32_t used_descriptor_sets_mask;
	bool uses_push_consts;
	
	struct {
		bool uses_base_vertex;
		bool uses_start_instance;
		bool uses_draw_index; // also in system values
	} vs;
	struct {
		bool needs_sample_positions;
	} ps;
	struct {
		uint32_t grid_size_components;
	} cs;
};
	
void
ac_nir_shader_info_pass(struct nir_shader *nir,
			struct ac_shader_info *info);
#endif
