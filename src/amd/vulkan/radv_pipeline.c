/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 *
 * based in part on anv driver which is:
 * Copyright © 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "util/mesa-sha1.h"
#include "radv_private.h"
#include "nir/nir.h"
#include "spirv/nir_spirv.h"

#include <llvm-c/Core.h>
#include <llvm-c/TargetMachine.h>

#include "sid.h"
#include "ac_binary.h"
#include "ac_llvm_util.h"
#include "ac_nir_to_llvm.h"
#include "vk_format.h"
static void radv_shader_variant_destroy(struct radv_device *device,
                                        struct radv_shader_variant *variant);

static const struct nir_shader_compiler_options nir_options = {
	.vertex_id_zero_based = true
};

VkResult radv_CreateShaderModule(
	VkDevice                                    _device,
	const VkShaderModuleCreateInfo*             pCreateInfo,
	const VkAllocationCallbacks*                pAllocator,
	VkShaderModule*                             pShaderModule)
{
	RADV_FROM_HANDLE(radv_device, device, _device);
	struct radv_shader_module *module;

	assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
	assert(pCreateInfo->flags == 0);

	module = radv_alloc2(&device->alloc, pAllocator,
			     sizeof(*module) + pCreateInfo->codeSize, 8,
			     VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (module == NULL)
		return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

	module->nir = NULL;
	module->size = pCreateInfo->codeSize;
	memcpy(module->data, pCreateInfo->pCode, module->size);

	_mesa_sha1_compute(module->data, module->size, module->sha1);

	*pShaderModule = radv_shader_module_to_handle(module);

	return VK_SUCCESS;
}

void radv_DestroyShaderModule(
	VkDevice                                    _device,
	VkShaderModule                              _module,
	const VkAllocationCallbacks*                pAllocator)
{
	RADV_FROM_HANDLE(radv_device, device, _device);
	RADV_FROM_HANDLE(radv_shader_module, module, _module);

	radv_free2(&device->alloc, pAllocator, module);
}

void radv_DestroyPipeline(
	VkDevice                                    _device,
	VkPipeline                                  _pipeline,
	const VkAllocationCallbacks*                pAllocator)
{
	RADV_FROM_HANDLE(radv_device, device, _device);
	RADV_FROM_HANDLE(radv_pipeline, pipeline, _pipeline);

	if (!_pipeline)
		return;
#if 0
	radv_reloc_list_finish(&pipeline->batch_relocs,
			       pAllocator ? pAllocator : &device->alloc);
	if (pipeline->blend_state.map)
		radv_state_pool_free(&device->dynamic_state_pool, pipeline->blend_state);
#endif
	for (unsigned i = 0; i < MESA_SHADER_STAGES; ++i)
		if (pipeline->shaders[i])
			radv_shader_variant_destroy(device, pipeline->shaders[i]);

	radv_free2(&device->alloc, pAllocator, pipeline);
}

static nir_shader *
radv_shader_compile_to_nir(struct radv_device *device,
			   struct radv_shader_module *module,
			   const char *entrypoint_name,
			   gl_shader_stage stage,
			   const VkSpecializationInfo *spec_info)
{
	if (strcmp(entrypoint_name, "main") != 0) {
		radv_finishme("Multiple shaders per module not really supported");
	}

#if 0
	const struct brw_compiler *compiler =
		device->instance->physicalDevice.compiler;
#endif
	nir_shader *nir;
	nir_function *entry_point;
	if (module->nir) {
		/* Some things such as our meta clear/blit code will give us a NIR
		 * shader directly.  In that case, we just ignore the SPIR-V entirely
		 * and just use the NIR shader */
		nir = module->nir;
		nir->options = &nir_options;
		nir_validate_shader(nir);

		assert(exec_list_length(&nir->functions) == 1);
		struct exec_node *node = exec_list_get_head(&nir->functions);
		entry_point = exec_node_data(nir_function, node, node);
	} else {
		uint32_t *spirv = (uint32_t *) module->data;
		assert(module->size % 4 == 0);

		uint32_t num_spec_entries = 0;
		struct nir_spirv_specialization *spec_entries = NULL;
		if (spec_info && spec_info->mapEntryCount > 0) {
			num_spec_entries = spec_info->mapEntryCount;
			spec_entries = malloc(num_spec_entries * sizeof(*spec_entries));
			for (uint32_t i = 0; i < num_spec_entries; i++) {
				VkSpecializationMapEntry entry = spec_info->pMapEntries[i];
				const void *data = spec_info->pData + entry.offset;
				assert(data + entry.size <= spec_info->pData + spec_info->dataSize);

				spec_entries[i].id = spec_info->pMapEntries[i].constantID;
				spec_entries[i].data = *(const uint32_t *)data;
			}
		}

		entry_point = spirv_to_nir(spirv, module->size / 4,
					   spec_entries, num_spec_entries,
					   stage, entrypoint_name, &nir_options);
		nir = entry_point->shader;
		assert(nir->stage == stage);
		nir_validate_shader(nir);

		free(spec_entries);


		if (stage == MESA_SHADER_FRAGMENT) {
			nir_lower_wpos_center(nir);
			nir_validate_shader(nir);
		}

		nir_lower_returns(nir);
		nir_validate_shader(nir);

		nir_inline_functions(nir);
		nir_validate_shader(nir);

		/* Pick off the single entrypoint that we want */
		foreach_list_typed_safe(nir_function, func, node, &nir->functions) {
			if (func != entry_point)
				exec_node_remove(&func->node);
		}
		assert(exec_list_length(&nir->functions) == 1);
		entry_point->name = ralloc_strdup(entry_point, "main");

		nir_remove_dead_variables(nir, nir_var_shader_in);
		nir_remove_dead_variables(nir, nir_var_shader_out);
		nir_remove_dead_variables(nir, nir_var_system_value);
		nir_validate_shader(nir);

		nir_lower_io_to_temporaries(entry_point->shader, entry_point, true, false);

		nir_lower_system_values(nir);
		nir_validate_shader(nir);
		//      nir_print_shader(nir, stderr);
	}

	/* Vulkan uses the separate-shader linking model */
	nir->info.separate_shader = true;

	//   nir = brw_preprocess_nir(compiler, nir);

	nir_shader_gather_info(nir, entry_point->impl);

	nir_variable_mode indirect_mask = 0;
	//   if (compiler->glsl_compiler_options[stage].EmitNoIndirectInput)
	indirect_mask |= nir_var_shader_in;
	//   if (compiler->glsl_compiler_options[stage].EmitNoIndirectTemp)
	indirect_mask |= nir_var_local;

	nir_lower_indirect_derefs(nir, indirect_mask);
	nir_lower_vars_to_ssa(nir);
	nir_lower_var_copies(nir);
	nir_lower_global_vars_to_local(nir);
	nir_remove_dead_variables(nir, nir_var_local);
	nir_lower_alu_to_scalar(nir);
	nir_print_shader(nir, stderr);
   
	return nir;
}

static void radv_shader_variant_destroy(struct radv_device *device,
                                        struct radv_shader_variant *variant)
{
	device->ws->buffer_destroy(variant->bo);
	free(variant);
}

static
struct radv_shader_variant *radv_shader_variant_create(struct radv_device *device,
                                                       struct nir_shader *shader,
                                                       struct radv_pipeline_layout *layout)
{
	struct radv_shader_variant *variant = calloc(1, sizeof(struct radv_shader_variant));
	if (!variant)
		return NULL;

	struct ac_nir_compiler_options options = {0};
	options.layout = layout;
	struct ac_shader_binary binary;
	ac_compile_nir_shader(device->target_machine, &binary, &variant->config,
			      &variant->info, shader, &options);

	bool scratch_enabled = variant->config.scratch_bytes_per_wave > 0;
	unsigned vgpr_comp_cnt = 0;
	switch (shader->stage) {
	case MESA_SHADER_VERTEX:
		variant->rsrc2 = S_00B12C_USER_SGPR(variant->info.num_user_sgprs) |
			S_00B12C_SCRATCH_EN(scratch_enabled);
		break;
	case MESA_SHADER_FRAGMENT:
		variant->rsrc2 = S_00B12C_USER_SGPR(variant->info.num_user_sgprs) |
			S_00B12C_SCRATCH_EN(scratch_enabled);
		break;
	case MESA_SHADER_COMPUTE:
		variant->rsrc2 = S_00B84C_USER_SGPR(variant->info.num_user_sgprs) |
			S_00B84C_SCRATCH_EN(scratch_enabled) |
			S_00B84C_TGID_X_EN(1) | S_00B84C_TGID_Y_EN(1) |
			S_00B84C_TGID_Z_EN(1) | S_00B84C_TIDIG_COMP_CNT(2) |
			S_00B84C_LDS_SIZE(variant->config.lds_size);
		break;
	}

	variant->rsrc1 =  S_00B848_VGPRS((variant->config.num_vgprs - 1) / 4) |
		S_00B848_SGPRS((variant->config.num_sgprs - 1) / 8) |
		S_00B128_VGPR_COMP_CNT(vgpr_comp_cnt) |
		S_00B848_DX10_CLAMP(1) |
		S_00B848_FLOAT_MODE(variant->config.float_mode);

	variant->bo = device->ws->buffer_create(device->ws, binary.code_size, 256,
						RADEON_DOMAIN_GTT, RADEON_FLAG_CPU_ACCESS);

	void *ptr = device->ws->buffer_map(variant->bo);
	memcpy(ptr, binary.code, binary.code_size);
	device->ws->buffer_unmap(variant->bo);
	return variant;
}


static nir_shader *
radv_pipeline_compile(struct radv_pipeline *pipeline,
		      struct radv_shader_module *module,
		      const char *entrypoint,
		      gl_shader_stage stage,
		      const VkSpecializationInfo *spec_info)
{
	nir_shader *nir = radv_shader_compile_to_nir(pipeline->device,
						     module, entrypoint, stage,
						     spec_info);
	if (nir == NULL)
		return NULL;

	return nir;
}


static void
radv_pipeline_init_blend_state(struct radv_pipeline *pipeline,
			       const VkGraphicsPipelineCreateInfo *pCreateInfo)
{
	const VkPipelineColorBlendStateCreateInfo *vkblend = pCreateInfo->pColorBlendState;
	struct radv_blend_state *blend = &pipeline->graphics.blend;
	unsigned mode = V_028808_CB_NORMAL;
	int i;

	blend->cb_color_control = 0;
	if (vkblend->logicOpEnable)
		blend->cb_color_control |= S_028808_ROP3(vkblend->logicOp | (vkblend->logicOp << 4));
	else
		blend->cb_color_control |= S_028808_ROP3(0xcc);

	blend->cb_target_mask = 0;
	for (i = 0; i < vkblend->attachmentCount; i++) {
		const VkPipelineColorBlendAttachmentState *att = &vkblend->pAttachments[i];
		unsigned blend_cntl = 0;
		blend->sx_mrt0_blend_opt[i] = S_028760_COLOR_COMB_FCN(V_028760_OPT_COMB_BLEND_DISABLED) | S_028760_ALPHA_COMB_FCN(V_028760_OPT_COMB_BLEND_DISABLED);

		if (!att->colorWriteMask)
			continue;

		blend->cb_target_mask |= (unsigned)att->colorWriteMask << (4 * i);
		if (!att->blendEnable) {
			blend->cb_blend_control[i] = blend_cntl;
			continue;
		}

		blend_cntl |= S_028780_ENABLE(1);

		blend->cb_blend_control[i] = blend_cntl;
	}
	for (i = vkblend->attachmentCount; i < 8; i++)
		blend->cb_blend_control[i] = 0;

	if (blend->cb_target_mask)
		blend->cb_color_control |= S_028808_MODE(mode);
	else
		blend->cb_color_control |= S_028808_MODE(V_028808_CB_DISABLE);
}

static uint32_t si_translate_stencil_op(enum VkStencilOp op)
{
	switch (op) {
	case VK_STENCIL_OP_KEEP:
		return V_02842C_STENCIL_KEEP;
	case VK_STENCIL_OP_ZERO:
		return V_02842C_STENCIL_ZERO;
	case VK_STENCIL_OP_REPLACE:
		return V_02842C_STENCIL_REPLACE_TEST;
	case VK_STENCIL_OP_INCREMENT_AND_CLAMP:
		return V_02842C_STENCIL_ADD_CLAMP;
	case VK_STENCIL_OP_DECREMENT_AND_CLAMP:
		return V_02842C_STENCIL_SUB_CLAMP;
	case VK_STENCIL_OP_INVERT:
		return V_02842C_STENCIL_INVERT;
	case VK_STENCIL_OP_INCREMENT_AND_WRAP:
		return V_02842C_STENCIL_ADD_WRAP;
	case VK_STENCIL_OP_DECREMENT_AND_WRAP:
		return V_02842C_STENCIL_SUB_WRAP;
	default:
		return 0;
	}
}
static void
radv_pipeline_init_depth_stencil_state(struct radv_pipeline *pipeline,
				       const VkGraphicsPipelineCreateInfo *pCreateInfo)
{
	const VkPipelineDepthStencilStateCreateInfo *vkds = pCreateInfo->pDepthStencilState;
	struct radv_depth_stencil_state *ds = &pipeline->graphics.ds;

	memset(ds, 0, sizeof(*ds));
	if (!vkds)
		return;
	ds->db_depth_control = S_028800_Z_ENABLE(vkds->depthTestEnable ? 1 : 0) |
		S_028800_Z_WRITE_ENABLE(vkds->depthWriteEnable ? 1 : 0) |
		S_028800_ZFUNC(vkds->depthCompareOp) |
		S_028800_DEPTH_BOUNDS_ENABLE(vkds->depthBoundsTestEnable ? 1 : 0);

	if (vkds->stencilTestEnable) {
		ds->db_depth_control |= S_028800_STENCIL_ENABLE(1) | S_028800_BACKFACE_ENABLE(1);
		ds->db_depth_control |= S_028800_STENCILFUNC(vkds->front.compareOp);
		ds->db_stencil_control |= S_02842C_STENCILFAIL(si_translate_stencil_op(vkds->front.failOp));
		ds->db_stencil_control |= S_02842C_STENCILZPASS(si_translate_stencil_op(vkds->front.passOp));
		ds->db_stencil_control |= S_02842C_STENCILZFAIL(si_translate_stencil_op(vkds->front.depthFailOp));

		ds->db_depth_control |= S_028800_STENCILFUNC_BF(vkds->back.compareOp);
		ds->db_stencil_control |= S_02842C_STENCILFAIL_BF(si_translate_stencil_op(vkds->back.failOp));
		ds->db_stencil_control |= S_02842C_STENCILZPASS_BF(si_translate_stencil_op(vkds->back.passOp));
		ds->db_stencil_control |= S_02842C_STENCILZFAIL_BF(si_translate_stencil_op(vkds->back.depthFailOp));
	}

	ds->db_depth_bounds_min = fui(vkds->minDepthBounds);
	ds->db_depth_bounds_max = fui(vkds->maxDepthBounds);
}

static uint32_t si_translate_fill(VkPolygonMode func)
{
	switch(func) {
	case VK_POLYGON_MODE_FILL:
		return V_028814_X_DRAW_TRIANGLES;
	case VK_POLYGON_MODE_LINE:
		return V_028814_X_DRAW_LINES;
	case VK_POLYGON_MODE_POINT:
		return V_028814_X_DRAW_POINTS;
	default:
		assert(0);
		return V_028814_X_DRAW_POINTS;
	}
}
static void
radv_pipeline_init_raster_state(struct radv_pipeline *pipeline,
				const VkGraphicsPipelineCreateInfo *pCreateInfo)
{
	const VkPipelineRasterizationStateCreateInfo *vkraster = pCreateInfo->pRasterizationState;
	struct radv_raster_state *raster = &pipeline->graphics.raster;

	memset(raster, 0, sizeof(*raster));

	raster->spi_interp_control =
		S_0286D4_FLAT_SHADE_ENA(1) |
		S_0286D4_PNT_SPRITE_ENA(1) |
		S_0286D4_PNT_SPRITE_OVRD_X(V_0286D4_SPI_PNT_SPRITE_SEL_S) |
		S_0286D4_PNT_SPRITE_OVRD_Y(V_0286D4_SPI_PNT_SPRITE_SEL_T) |
		S_0286D4_PNT_SPRITE_OVRD_Z(V_0286D4_SPI_PNT_SPRITE_SEL_0) |
		S_0286D4_PNT_SPRITE_OVRD_W(V_0286D4_SPI_PNT_SPRITE_SEL_1) |
		S_0286D4_PNT_SPRITE_TOP_1(0); // TODO verify

	raster->pa_cl_vs_out_cntl = S_02881C_VS_OUT_MISC_SIDE_BUS_ENA(1);
	raster->pa_cl_clip_cntl = S_028810_PS_UCP_MODE(3) |
		S_028810_DX_CLIP_SPACE_DEF(0) | // TODO verify
		S_028810_ZCLIP_NEAR_DISABLE(vkraster->depthClampEnable ? 1 : 0) |
		S_028810_ZCLIP_FAR_DISABLE(vkraster->depthClampEnable ? 1 : 0) |
		S_028810_DX_RASTERIZATION_KILL(vkraster->rasterizerDiscardEnable ? 1 : 0) |
		S_028810_DX_LINEAR_ATTR_CLIP_ENA(1);

	raster->pa_su_vtx_cntl =
		S_028BE4_PIX_CENTER(1) | // TODO verify
		S_028BE4_QUANT_MODE(V_028BE4_X_16_8_FIXED_POINT_1_256TH);

	raster->pa_sc_mode_cntl_0 = S_028A48_VPORT_SCISSOR_ENABLE(1);
	raster->pa_su_sc_mode_cntl =
		S_028814_FACE(vkraster->frontFace) |
		S_028814_CULL_FRONT(vkraster->cullMode & VK_CULL_MODE_FRONT_BIT) |
		S_028814_CULL_BACK(vkraster->cullMode & VK_CULL_MODE_BACK_BIT) |
		S_028814_POLYMODE_FRONT_PTYPE(si_translate_fill(vkraster->polygonMode)) |
		S_028814_POLYMODE_BACK_PTYPE(si_translate_fill(vkraster->polygonMode));
}

static uint32_t
si_translate_prim(enum VkPrimitiveTopology topology)
{
	switch (topology) {
	case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
		return V_008958_DI_PT_POINTLIST;
	case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
		return V_008958_DI_PT_LINELIST;
	case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
		return V_008958_DI_PT_LINESTRIP;
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
		return V_008958_DI_PT_TRILIST;
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
		return V_008958_DI_PT_TRISTRIP;
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
		return V_008958_DI_PT_TRIFAN;
	case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY:
		return V_008958_DI_PT_LINELIST_ADJ;
	case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY:
		return V_008958_DI_PT_LINESTRIP_ADJ;
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY:
		return V_008958_DI_PT_TRILIST_ADJ;
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY:
		return V_008958_DI_PT_TRISTRIP_ADJ;
	case VK_PRIMITIVE_TOPOLOGY_PATCH_LIST:
		return V_008958_DI_PT_PATCH;
	default:
		assert(0);
		return 0;
	}
}

static unsigned si_map_swizzle(unsigned swizzle)
{
	switch (swizzle) {
	case VK_SWIZZLE_Y:
		return V_008F0C_SQ_SEL_Y;
	case VK_SWIZZLE_Z:
		return V_008F0C_SQ_SEL_Z;
	case VK_SWIZZLE_W:
		return V_008F0C_SQ_SEL_W;
	case VK_SWIZZLE_0:
		return V_008F0C_SQ_SEL_0;
	case VK_SWIZZLE_1:
		return V_008F0C_SQ_SEL_1;
	default: /* VK_SWIZZLE_X */
		return V_008F0C_SQ_SEL_X;
	}
}

static void
radv_pipeline_init_dynamic_state(struct radv_pipeline *pipeline,
				 const VkGraphicsPipelineCreateInfo *pCreateInfo)
{
	radv_cmd_dirty_mask_t states = RADV_CMD_DIRTY_DYNAMIC_ALL;
	RADV_FROM_HANDLE(radv_render_pass, pass, pCreateInfo->renderPass);
	struct radv_subpass *subpass = &pass->subpasses[pCreateInfo->subpass];

	pipeline->dynamic_state = default_dynamic_state;

	if (pCreateInfo->pDynamicState) {
		/* Remove all of the states that are marked as dynamic */
		uint32_t count = pCreateInfo->pDynamicState->dynamicStateCount;
		for (uint32_t s = 0; s < count; s++)
			states &= ~(1 << pCreateInfo->pDynamicState->pDynamicStates[s]);
	}

	struct radv_dynamic_state *dynamic = &pipeline->dynamic_state;

	dynamic->viewport.count = pCreateInfo->pViewportState->viewportCount;
	if (states & (1 << VK_DYNAMIC_STATE_VIEWPORT)) {
		typed_memcpy(dynamic->viewport.viewports,
			     pCreateInfo->pViewportState->pViewports,
			     pCreateInfo->pViewportState->viewportCount);
	}

	dynamic->scissor.count = pCreateInfo->pViewportState->scissorCount;
	if (states & (1 << VK_DYNAMIC_STATE_SCISSOR)) {
		typed_memcpy(dynamic->scissor.scissors,
			     pCreateInfo->pViewportState->pScissors,
			     pCreateInfo->pViewportState->scissorCount);
	}

	if (states & (1 << VK_DYNAMIC_STATE_LINE_WIDTH)) {
		assert(pCreateInfo->pRasterizationState);
		dynamic->line_width = pCreateInfo->pRasterizationState->lineWidth;
	}

	if (states & (1 << VK_DYNAMIC_STATE_DEPTH_BIAS)) {
		assert(pCreateInfo->pRasterizationState);
		dynamic->depth_bias.bias =
			pCreateInfo->pRasterizationState->depthBiasConstantFactor;
		dynamic->depth_bias.clamp =
			pCreateInfo->pRasterizationState->depthBiasClamp;
		dynamic->depth_bias.slope =
			pCreateInfo->pRasterizationState->depthBiasSlopeFactor;
	}

	if (states & (1 << VK_DYNAMIC_STATE_BLEND_CONSTANTS)) {
		assert(pCreateInfo->pColorBlendState);
		typed_memcpy(dynamic->blend_constants,
			     pCreateInfo->pColorBlendState->blendConstants, 4);
	}

	/* If there is no depthstencil attachment, then don't read
	 * pDepthStencilState. The Vulkan spec states that pDepthStencilState may
	 * be NULL in this case. Even if pDepthStencilState is non-NULL, there is
	 * no need to override the depthstencil defaults in
	 * radv_pipeline::dynamic_state when there is no depthstencil attachment.
	 *
	 * From the Vulkan spec (20 Oct 2015, git-aa308cb):
	 *
	 *    pDepthStencilState [...] may only be NULL if renderPass and subpass
	 *    specify a subpass that has no depth/stencil attachment.
	 */
	if (subpass->depth_stencil_attachment != VK_ATTACHMENT_UNUSED) {
		if (states & (1 << VK_DYNAMIC_STATE_DEPTH_BOUNDS)) {
			assert(pCreateInfo->pDepthStencilState);
			dynamic->depth_bounds.min =
				pCreateInfo->pDepthStencilState->minDepthBounds;
			dynamic->depth_bounds.max =
				pCreateInfo->pDepthStencilState->maxDepthBounds;
		}

		if (states & (1 << VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK)) {
			assert(pCreateInfo->pDepthStencilState);
			dynamic->stencil_compare_mask.front =
				pCreateInfo->pDepthStencilState->front.compareMask;
			dynamic->stencil_compare_mask.back =
				pCreateInfo->pDepthStencilState->back.compareMask;
		}

		if (states & (1 << VK_DYNAMIC_STATE_STENCIL_WRITE_MASK)) {
			assert(pCreateInfo->pDepthStencilState);
			dynamic->stencil_write_mask.front =
				pCreateInfo->pDepthStencilState->front.writeMask;
			dynamic->stencil_write_mask.back =
				pCreateInfo->pDepthStencilState->back.writeMask;
		}

		if (states & (1 << VK_DYNAMIC_STATE_STENCIL_REFERENCE)) {
			assert(pCreateInfo->pDepthStencilState);
			dynamic->stencil_reference.front =
				pCreateInfo->pDepthStencilState->front.reference;
			dynamic->stencil_reference.back =
				pCreateInfo->pDepthStencilState->back.reference;
		}
	}

	pipeline->dynamic_state_mask = states;
}

VkResult
radv_pipeline_init(struct radv_pipeline *pipeline,
		   struct radv_device *device,
		   struct radv_pipeline_cache *cache,
		   const VkGraphicsPipelineCreateInfo *pCreateInfo,
		   const struct radv_graphics_pipeline_create_info *extra,
		   const VkAllocationCallbacks *alloc)
{
	VkResult result;
	struct nir_shader *shader;
	if (alloc == NULL)
		alloc = &device->alloc;

	pipeline->device = device;
	pipeline->layout = radv_pipeline_layout_from_handle(pCreateInfo->layout);

	radv_pipeline_init_dynamic_state(pipeline, pCreateInfo);
	const VkPipelineShaderStageCreateInfo *pStages[MESA_SHADER_STAGES] = { 0, };
	struct radv_shader_module *modules[MESA_SHADER_STAGES] = { 0, };
	for (uint32_t i = 0; i < pCreateInfo->stageCount; i++) {
		gl_shader_stage stage = ffs(pCreateInfo->pStages[i].stage) - 1;
		pStages[stage] = &pCreateInfo->pStages[i];
		modules[stage] = radv_shader_module_from_handle(pStages[stage]->module);
	}

	/* */
	if (modules[MESA_SHADER_VERTEX]) {
		shader = radv_pipeline_compile(pipeline, modules[MESA_SHADER_VERTEX],
					       pStages[MESA_SHADER_VERTEX]->pName,
					       MESA_SHADER_VERTEX,
					       pStages[MESA_SHADER_VERTEX]->pSpecializationInfo);
		pipeline->shaders[MESA_SHADER_VERTEX] = radv_shader_variant_create(device,
										   shader,
										   pipeline->layout);
		fprintf(stderr, "Assign VS %p to %p\n", pipeline->shaders[MESA_SHADER_VERTEX], pipeline);
		pipeline->active_stages |= mesa_to_vk_shader_stage(MESA_SHADER_VERTEX);
	}

	if (modules[MESA_SHADER_FRAGMENT]) {
		shader = radv_pipeline_compile(pipeline, modules[MESA_SHADER_FRAGMENT],
					       pStages[MESA_SHADER_FRAGMENT]->pName,
					       MESA_SHADER_FRAGMENT,
					       pStages[MESA_SHADER_FRAGMENT]->pSpecializationInfo);
		pipeline->shaders[MESA_SHADER_FRAGMENT] = radv_shader_variant_create(device,
										     shader,
										     pipeline->layout);
		fprintf(stderr, "Assign FS %p to %p\n", pipeline->shaders[MESA_SHADER_FRAGMENT], pipeline);
		pipeline->active_stages |= mesa_to_vk_shader_stage(MESA_SHADER_FRAGMENT);
	}

	radv_pipeline_init_blend_state(pipeline, pCreateInfo);
	radv_pipeline_init_depth_stencil_state(pipeline, pCreateInfo);
	radv_pipeline_init_raster_state(pipeline, pCreateInfo);

	pipeline->graphics.prim = si_translate_prim(pCreateInfo->pInputAssemblyState->topology);
	if (extra && extra->use_rectlist)
		pipeline->graphics.prim = V_008958_DI_PT_RECTLIST;
	pipeline->graphics.prim_restart_enable = pCreateInfo->pInputAssemblyState->primitiveRestartEnable;

	const VkPipelineVertexInputStateCreateInfo *vi_info =
		pCreateInfo->pVertexInputState;
	for (uint32_t i = 0; i < vi_info->vertexAttributeDescriptionCount; i++) {
		const VkVertexInputAttributeDescription *desc =
			&vi_info->pVertexAttributeDescriptions[i];
		const struct vk_format_description *format_desc;
		int first_non_void;
		uint32_t num_format, data_format;
		format_desc = vk_format_description(desc->format);
		first_non_void = vk_format_get_first_non_void_channel(desc->format);

		num_format = radv_translate_buffer_numformat(format_desc, first_non_void);
		data_format = radv_translate_buffer_dataformat(format_desc, first_non_void);

		pipeline->va_rsrc_word3[i] = S_008F0C_DST_SEL_X(si_map_swizzle(format_desc->swizzle[0])) |
			S_008F0C_DST_SEL_Y(si_map_swizzle(format_desc->swizzle[1])) |
			S_008F0C_DST_SEL_Z(si_map_swizzle(format_desc->swizzle[2])) |
			S_008F0C_DST_SEL_W(si_map_swizzle(format_desc->swizzle[3])) |
			S_008F0C_NUM_FORMAT(num_format) |
			S_008F0C_DATA_FORMAT(data_format);

		pipeline->va_offset[i] = desc->offset;
		pipeline->va_binding[i] = desc->binding;
	}
	pipeline->num_vertex_attribs = vi_info->vertexAttributeDescriptionCount;
	for (uint32_t i = 0; i < vi_info->vertexBindingDescriptionCount; i++) {
		const VkVertexInputBindingDescription *desc =
			&vi_info->pVertexBindingDescriptions[i];

		pipeline->binding_stride[desc->binding] = desc->stride;

		/* Step rate is programmed per vertex element (attribute), not
		 * binding. Set up a map of which bindings step per instance, for
		 * reference by vertex element setup. */
		switch (desc->inputRate) {
		default:
		case VK_VERTEX_INPUT_RATE_VERTEX:
			pipeline->instancing_enable[desc->binding] = false;
			break;
		case VK_VERTEX_INPUT_RATE_INSTANCE:
			pipeline->instancing_enable[desc->binding] = true;
			break;
		}
	}

	return VK_SUCCESS;
}

VkResult
radv_graphics_pipeline_create(
	VkDevice _device,
	VkPipelineCache _cache,
	const VkGraphicsPipelineCreateInfo *pCreateInfo,
	const struct radv_graphics_pipeline_create_info *extra,
	const VkAllocationCallbacks *pAllocator,
	VkPipeline *pPipeline)
{
	RADV_FROM_HANDLE(radv_device, device, _device);
	RADV_FROM_HANDLE(radv_pipeline_cache, cache, _cache);
	struct radv_pipeline *pipeline;
	VkResult result;

	//   if (cache == NULL)
	//      cache = &device->default_pipeline_cache;
	pipeline = radv_alloc2(&device->alloc, pAllocator, sizeof(*pipeline), 8,
			       VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (pipeline == NULL)
		return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

	memset(pipeline, 0, sizeof(*pipeline));
	result = radv_pipeline_init(pipeline, device, cache,
				    pCreateInfo, extra, pAllocator);
	if (result != VK_SUCCESS) {
		radv_free2(&device->alloc, pAllocator, pipeline);
		return result;
	}

	*pPipeline = radv_pipeline_to_handle(pipeline);

	return VK_SUCCESS;
}

VkResult radv_CreateGraphicsPipelines(
	VkDevice                                    _device,
	VkPipelineCache                             pipelineCache,
	uint32_t                                    count,
	const VkGraphicsPipelineCreateInfo*         pCreateInfos,
	const VkAllocationCallbacks*                pAllocator,
	VkPipeline*                                 pPipelines)
{
	VkResult result = VK_SUCCESS;
	unsigned i = 0;

	for (; i < count; i++) {
		result = radv_graphics_pipeline_create(_device,
						       pipelineCache,
						       &pCreateInfos[i],
						       NULL, pAllocator, &pPipelines[i]);
		if (result != VK_SUCCESS) {
			for (unsigned j = 0; j < i; j++) {
				radv_DestroyPipeline(_device, pPipelines[j], pAllocator);
			}

			return result;
		}
	}

	return VK_SUCCESS;
}

static VkResult radv_compute_pipeline_create(
	VkDevice                                    _device,
	VkPipelineCache                             _cache,
	const VkComputePipelineCreateInfo*          pCreateInfo,
	const VkAllocationCallbacks*                pAllocator,
	VkPipeline*                                 pPipeline)
{
	RADV_FROM_HANDLE(radv_device, device, _device);
	RADV_FROM_HANDLE(radv_pipeline_cache, cache, _cache);
	RADV_FROM_HANDLE(radv_shader_module, module, pCreateInfo->stage.module);
	struct radv_pipeline *pipeline;
	struct nir_shader *shader;

	//   if (cache == NULL)
	//      cache = &device->default_pipeline_cache;

	pipeline = radv_alloc2(&device->alloc, pAllocator, sizeof(*pipeline), 8,
			       VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);

	pipeline->device = device;
	pipeline->layout = radv_pipeline_layout_from_handle(pCreateInfo->layout);

	shader = radv_pipeline_compile(pipeline, module,
				       pCreateInfo->stage.pName,
				       MESA_SHADER_COMPUTE,
				       pCreateInfo->stage.pSpecializationInfo);

	for (int i = 0; i < 3; ++i)
		pipeline->compute.block_size[i] = shader->info.cs.local_size[i];

	pipeline->shaders[MESA_SHADER_COMPUTE] = radv_shader_variant_create(device,
									    shader,
									    pipeline->layout);

	ralloc_free(shader);
	*pPipeline = radv_pipeline_to_handle(pipeline);
	return VK_SUCCESS;
}
VkResult radv_CreateComputePipelines(
	VkDevice                                    _device,
	VkPipelineCache                             pipelineCache,
	uint32_t                                    count,
	const VkComputePipelineCreateInfo*          pCreateInfos,
	const VkAllocationCallbacks*                pAllocator,
	VkPipeline*                                 pPipelines)
{
	VkResult result = VK_SUCCESS;

	unsigned i = 0;
	for (; i < count; i++) {
		result = radv_compute_pipeline_create(_device, pipelineCache,
						      &pCreateInfos[i],
						      pAllocator, &pPipelines[i]);
		if (result != VK_SUCCESS) {
			for (unsigned j = 0; j < i; j++) {
				radv_DestroyPipeline(_device, pPipelines[j], pAllocator);
			}

			return result;
		}
	}

	return VK_SUCCESS;
}
