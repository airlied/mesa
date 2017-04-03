
#include "nir.h"
#include "ac_shader_info.h"
/* a NIR pass to gather all the info needed to optimise the alloction patterns for the RADV user sgprs */
/* also can be used to get other info we need to optimise in other areas */
/*
 * SCRATCH_RING_OFFSETS - always enabled for all stages with later LLVM. SGPRs 0/1.
 * DESCRIPTOR SETS - needed if shader access any of the sets.
 * PUSH_CONSTANTS - needed if we have push constants or dynamic offsets - we should allow for inlining some
 *                  push constants if there are left over user sgprs.
 * VS:
 * VERTEX_BUFFERS - needed if the vertex shader fetches from an input
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


static void
gather_vulkan_resource_index(nir_intrinsic_instr *instr,
			     struct ac_shader_info *info)
{
	unsigned desc_set = nir_intrinsic_desc_set(instr);

	info->used_descriptor_sets_mask |= (1 << desc_set);
}

static void
gather_intrinsic_info(nir_intrinsic_instr *instr, struct ac_shader_info *info)
{
	switch (instr->intrinsic) {
	case nir_intrinsic_vulkan_resource_index:
		gather_vulkan_resource_index(instr, info);
		break;
	case nir_intrinsic_load_push_constant:
		info->uses_push_consts = true;
		break;
	case nir_intrinsic_interp_var_at_sample:
		info->ps.needs_sample_positions = true;
		break;
	case nir_intrinsic_load_num_work_groups:
		if (instr->num_components > info->cs.grid_size_components)
			info->cs.grid_size_components = instr->num_components;
		break;
	default:
		break;
	}
}
	
static void
gather_info_block(nir_block *block, struct ac_shader_info *info)
{
	nir_foreach_instr(instr, block) {
		switch (instr->type) {
		case nir_instr_type_intrinsic:
			gather_intrinsic_info(nir_instr_as_intrinsic(instr), info);
			break;
		default:
			break;
		}
	}
}
void
ac_nir_shader_info_pass(struct nir_shader *nir,
			struct ac_shader_info *info)
{
	struct nir_function *func = (struct nir_function *)exec_list_get_head(&nir->functions);

	nir_foreach_block(block, func->impl) {
		gather_info_block(block, info);
	}
		
}
