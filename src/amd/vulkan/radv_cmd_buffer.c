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

#include "radv_private.h"
#include "radv_radeon_winsys.h"
#include "radv_cs.h"
#include "sid.h"
#include "vk_format.h"

const struct radv_dynamic_state default_dynamic_state = {
	.viewport = {
		.count = 0,
	},
	.scissor = {
		.count = 0,
	},
	.line_width = 1.0f,
	.depth_bias = {
		.bias = 0.0f,
		.clamp = 0.0f,
		.slope = 0.0f,
	},
	.blend_constants = { 0.0f, 0.0f, 0.0f, 0.0f },
	.depth_bounds = {
		.min = 0.0f,
		.max = 1.0f,
	},
	.stencil_compare_mask = {
		.front = ~0u,
		.back = ~0u,
	},
	.stencil_write_mask = {
		.front = ~0u,
		.back = ~0u,
	},
	.stencil_reference = {
		.front = 0u,
		.back = 0u,
	},
};

void
radv_dynamic_state_copy(struct radv_dynamic_state *dest,
			const struct radv_dynamic_state *src,
			uint32_t copy_mask)
{
	if (copy_mask & (1 << VK_DYNAMIC_STATE_VIEWPORT)) {
		dest->viewport.count = src->viewport.count;
		typed_memcpy(dest->viewport.viewports, src->viewport.viewports,
			     src->viewport.count);
	}

	if (copy_mask & (1 << VK_DYNAMIC_STATE_SCISSOR)) {
		dest->scissor.count = src->scissor.count;
		typed_memcpy(dest->scissor.scissors, src->scissor.scissors,
			     src->scissor.count);
	}

	if (copy_mask & (1 << VK_DYNAMIC_STATE_LINE_WIDTH))
		dest->line_width = src->line_width;

	if (copy_mask & (1 << VK_DYNAMIC_STATE_DEPTH_BIAS))
		dest->depth_bias = src->depth_bias;

	if (copy_mask & (1 << VK_DYNAMIC_STATE_BLEND_CONSTANTS))
		typed_memcpy(dest->blend_constants, src->blend_constants, 4);

	if (copy_mask & (1 << VK_DYNAMIC_STATE_DEPTH_BOUNDS))
		dest->depth_bounds = src->depth_bounds;

	if (copy_mask & (1 << VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK))
		dest->stencil_compare_mask = src->stencil_compare_mask;

	if (copy_mask & (1 << VK_DYNAMIC_STATE_STENCIL_WRITE_MASK))
		dest->stencil_write_mask = src->stencil_write_mask;

	if (copy_mask & (1 << VK_DYNAMIC_STATE_STENCIL_REFERENCE))
		dest->stencil_reference = src->stencil_reference;
}

static VkResult radv_create_cmd_buffer(
	struct radv_device *                         device,
	struct radv_cmd_pool *                       pool,
	VkCommandBufferLevel                        level,
	VkCommandBuffer*                            pCommandBuffer)
{
	struct radv_cmd_buffer *cmd_buffer;
	VkResult result;

	cmd_buffer = radv_alloc(&pool->alloc, sizeof(*cmd_buffer), 8,
				VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (cmd_buffer == NULL)
		return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

	memset(cmd_buffer, 0, sizeof(*cmd_buffer));
	cmd_buffer->_loader_data.loaderMagic = ICD_LOADER_MAGIC;
	cmd_buffer->device = device;
	cmd_buffer->pool = pool;
	cmd_buffer->level = level;

	if (pool) {
		list_addtail(&cmd_buffer->pool_link, &pool->cmd_buffers);
	} else {
		/* Init the pool_link so we can safefly call list_del when we destroy
		 * the command buffer
		 */
		list_inithead(&cmd_buffer->pool_link);
	}

	cmd_buffer->cs = device->ws->cs_create(device->ws, RING_GFX);
	*pCommandBuffer = radv_cmd_buffer_to_handle(cmd_buffer);

	cmd_buffer->upload.upload_bo.bo = device->ws->buffer_create(device->ws,
								    RADV_CMD_BUFFER_UPLOAD_SIZE, 16,
								    RADEON_DOMAIN_GTT,
								    RADEON_FLAG_CPU_ACCESS);

	cmd_buffer->upload.map = device->ws->buffer_map(cmd_buffer->upload.upload_bo.bo);
	cmd_buffer->upload.offset = 0;

	device->ws->cs_add_buffer(cmd_buffer->cs, cmd_buffer->upload.upload_bo.bo, 8);

	cmd_buffer->border_color_bo.bo = device->ws->buffer_create(device->ws,
								   4096 * 4, 16,
								   RADEON_DOMAIN_VRAM,
								   RADEON_FLAG_CPU_ACCESS);
	device->ws->cs_add_buffer(cmd_buffer->cs, cmd_buffer->border_color_bo.bo, 8);
	return VK_SUCCESS;

fail:
	radv_free(&cmd_buffer->pool->alloc, cmd_buffer);

	return result;
}

void
radv_cmd_buffer_upload_alloc(struct radv_cmd_buffer *cmd_buffer,
			     unsigned size,
			     unsigned alignment,
			     unsigned *out_offset,
			     void **ptr)
{
	if (cmd_buffer->upload.offset + size > RADV_CMD_BUFFER_UPLOAD_SIZE) {
		fprintf(stderr, "time to implement larger upload buffer sizes.\n");
		exit(-1);
	}

	*out_offset = cmd_buffer->upload.offset;
	*ptr = cmd_buffer->upload.map + cmd_buffer->upload.offset;

	cmd_buffer->upload.offset += size;
}

void
radv_cmd_buffer_upload_data(struct radv_cmd_buffer *cmd_buffer,
			    unsigned size, unsigned alignment,
			    const void *data, unsigned *out_offset)
{
	uint8_t *ptr;

	radv_cmd_buffer_upload_alloc(cmd_buffer, size, alignment,
				     out_offset, (void **)&ptr);

	if (ptr)
		memcpy(ptr, data, size);
}

static void
radv_emit_graphics_blend_state(struct radv_cmd_buffer *cmd_buffer,
			       struct radv_pipeline *pipeline)
{
	radeon_set_context_reg_seq(cmd_buffer->cs, R_028780_CB_BLEND0_CONTROL, 8);
	radeon_emit_array(cmd_buffer->cs, pipeline->graphics.blend.cb_blend_control,
			  8);
	radeon_set_context_reg(cmd_buffer->cs, R_028808_CB_COLOR_CONTROL, pipeline->graphics.blend.cb_color_control);
}

static void
radv_emit_graphics_depth_stencil_state(struct radv_cmd_buffer *cmd_buffer,
				       struct radv_pipeline *pipeline)
{
	struct radv_depth_stencil_state *ds = &pipeline->graphics.ds;
	radeon_set_context_reg(cmd_buffer->cs, R_028800_DB_DEPTH_CONTROL, ds->db_depth_control);
	radeon_set_context_reg(cmd_buffer->cs, R_02842C_DB_STENCIL_CONTROL, ds->db_stencil_control);
	radeon_set_context_reg(cmd_buffer->cs, R_028020_DB_DEPTH_BOUNDS_MIN, ds->db_depth_bounds_min);
	radeon_set_context_reg(cmd_buffer->cs, R_028024_DB_DEPTH_BOUNDS_MAX, ds->db_depth_bounds_max);
}

static void
radv_emit_graphics_raster_state(struct radv_cmd_buffer *cmd_buffer,
				struct radv_pipeline *pipeline)
{
	struct radv_raster_state *raster = &pipeline->graphics.raster;

	radeon_set_context_reg(cmd_buffer->cs, R_028810_PA_CL_CLIP_CNTL,
			       raster->pa_cl_clip_cntl);
	radeon_set_context_reg(cmd_buffer->cs, R_028814_PA_SU_SC_MODE_CNTL,
			       raster->pa_su_sc_mode_cntl);
	radeon_set_context_reg(cmd_buffer->cs, R_02881C_PA_CL_VS_OUT_CNTL,
			       raster->pa_cl_vs_out_cntl);

	radeon_set_context_reg(cmd_buffer->cs, R_0286D4_SPI_INTERP_CONTROL_0,
			       raster->spi_interp_control);

	radeon_set_context_reg_seq(cmd_buffer->cs, R_028A00_PA_SU_POINT_SIZE, 2);
	radeon_emit(cmd_buffer->cs, 0);
	radeon_emit(cmd_buffer->cs, 0); /* R_028A04_PA_SU_POINT_MINMAX */

	radeon_set_context_reg(cmd_buffer->cs, R_028BE4_PA_SU_VTX_CNTL,
			       raster->pa_su_vtx_cntl);

	radeon_set_context_reg_seq(cmd_buffer->cs, R_028B80_PA_SU_POLY_OFFSET_FRONT_SCALE, 4);
	radeon_emit(cmd_buffer->cs, raster->pa_su_poly_offset_front_scale);
	radeon_emit(cmd_buffer->cs, raster->pa_su_poly_offset_front_offset);
	radeon_emit(cmd_buffer->cs, raster->pa_su_poly_offset_back_scale);
	radeon_emit(cmd_buffer->cs, raster->pa_su_poly_offset_back_offset);

	radeon_set_context_reg_seq(cmd_buffer->cs, CM_R_028BDC_PA_SC_LINE_CNTL, 2);
	radeon_emit(cmd_buffer->cs, S_028BDC_LAST_PIXEL(1));
	radeon_emit(cmd_buffer->cs, 0);

	radeon_set_context_reg(cmd_buffer->cs, CM_R_028804_DB_EQAA,
			       S_028804_HIGH_QUALITY_INTERSECTIONS(1) |
			       S_028804_STATIC_ANCHOR_ASSOCIATIONS(1));
	radeon_set_context_reg(cmd_buffer->cs, EG_R_028A4C_PA_SC_MODE_CNTL_1,
			       EG_S_028A4C_FORCE_EOV_CNTDWN_ENABLE(1) |
			       EG_S_028A4C_FORCE_EOV_REZ_ENABLE(1));
	radeon_set_context_reg(cmd_buffer->cs, R_028C38_PA_SC_AA_MASK_X0Y0_X1Y0, 0xffffffff);
	radeon_set_context_reg(cmd_buffer->cs, R_028C3C_PA_SC_AA_MASK_X0Y1_X1Y1, 0xffffffff);
}

static void
radv_emit_vertex_shader(struct radv_cmd_buffer *cmd_buffer,
			struct radv_pipeline *pipeline)
{
	struct radeon_winsys *ws = cmd_buffer->device->ws;
	struct radv_shader_variant *vs;
	uint64_t va;
	unsigned export_count;

	assert (pipeline->shaders[MESA_SHADER_VERTEX]);

	vs = pipeline->shaders[MESA_SHADER_VERTEX];
	va = ws->buffer_get_va(vs->bo);
	ws->cs_add_buffer(cmd_buffer->cs, vs->bo, 8);

	radeon_set_context_reg(cmd_buffer->cs, R_028A40_VGT_GS_MODE, 0);
	radeon_set_context_reg(cmd_buffer->cs, R_028A84_VGT_PRIMITIVEID_EN, 0);

	export_count = MAX2(1, vs->info.vs.param_exports);
	radeon_set_context_reg(cmd_buffer->cs, R_0286C4_SPI_VS_OUT_CONFIG,
			       S_0286C4_VS_EXPORT_COUNT(export_count - 1));
	radeon_set_context_reg(cmd_buffer->cs, R_02870C_SPI_SHADER_POS_FORMAT, S_02870C_POS0_EXPORT_FORMAT(V_02870C_SPI_SHADER_4COMP));

	radeon_set_sh_reg_seq(cmd_buffer->cs, R_00B120_SPI_SHADER_PGM_LO_VS, 4);
	radeon_emit(cmd_buffer->cs, va >> 8);
	radeon_emit(cmd_buffer->cs, va >> 40);
	radeon_emit(cmd_buffer->cs, vs->rsrc1);
	radeon_emit(cmd_buffer->cs, vs->rsrc2);

	radeon_set_context_reg(cmd_buffer->cs, R_028818_PA_CL_VTE_CNTL,
			       S_028818_VTX_W0_FMT(1) |
			       S_028818_VPORT_X_SCALE_ENA(1) | S_028818_VPORT_X_OFFSET_ENA(1) |
			       S_028818_VPORT_Y_SCALE_ENA(1) | S_028818_VPORT_Y_OFFSET_ENA(1) |
			       S_028818_VPORT_Z_SCALE_ENA(1) | S_028818_VPORT_Z_OFFSET_ENA(1));
}



static void
radv_emit_fragment_shader(struct radv_cmd_buffer *cmd_buffer,
			  struct radv_pipeline *pipeline)
{
	struct radeon_winsys *ws = cmd_buffer->device->ws;
	struct radv_shader_variant *ps, *vs;
	uint64_t va;
	unsigned spi_baryc_cntl = S_0286E0_FRONT_FACE_ALL_BITS(1);
	struct radv_blend_state *blend = &pipeline->graphics.blend;

	assert (pipeline->shaders[MESA_SHADER_FRAGMENT]);

	ps = pipeline->shaders[MESA_SHADER_FRAGMENT];
	vs = pipeline->shaders[MESA_SHADER_VERTEX];
	va = ws->buffer_get_va(ps->bo);
	ws->cs_add_buffer(cmd_buffer->cs, ps->bo, 8);

	radeon_set_sh_reg_seq(cmd_buffer->cs, R_00B020_SPI_SHADER_PGM_LO_PS, 4);
	radeon_emit(cmd_buffer->cs, va >> 8);
	radeon_emit(cmd_buffer->cs, va >> 40);
	radeon_emit(cmd_buffer->cs, ps->rsrc1);
	radeon_emit(cmd_buffer->cs, ps->rsrc2);

	radeon_set_context_reg(cmd_buffer->cs, R_028000_DB_RENDER_CONTROL, 0);
	radeon_set_context_reg(cmd_buffer->cs, R_028004_DB_COUNT_CONTROL, 0);
	radeon_set_context_reg(cmd_buffer->cs, R_028010_DB_RENDER_OVERRIDE2, 0);
	radeon_set_context_reg(cmd_buffer->cs, R_02880C_DB_SHADER_CONTROL,
			       S_02880C_KILL_ENABLE(!!ps->info.fs.can_discard) |
			       S_02880C_Z_ORDER(V_02880C_EARLY_Z_THEN_LATE_Z));

	radeon_set_context_reg(cmd_buffer->cs, R_0286CC_SPI_PS_INPUT_ENA,
			       ps->config.spi_ps_input_ena);

	radeon_set_context_reg(cmd_buffer->cs, R_0286D0_SPI_PS_INPUT_ADDR,
			       ps->config.spi_ps_input_addr);

	spi_baryc_cntl |= S_0286E0_POS_FLOAT_LOCATION(2);
	radeon_set_context_reg(cmd_buffer->cs, R_0286D8_SPI_PS_IN_CONTROL,
			       S_0286D8_NUM_INTERP(ps->info.fs.num_interp));

	radeon_set_context_reg(cmd_buffer->cs, R_0286E0_SPI_BARYC_CNTL, spi_baryc_cntl);

	radeon_set_context_reg(cmd_buffer->cs, R_028710_SPI_SHADER_Z_FORMAT, V_028710_SPI_SHADER_ZERO);

	radeon_set_context_reg(cmd_buffer->cs, R_028714_SPI_SHADER_COL_FORMAT, V_028714_SPI_SHADER_32_ABGR);

	radeon_set_context_reg(cmd_buffer->cs, R_028238_CB_TARGET_MASK, blend->cb_target_mask & 0xf);
	radeon_set_context_reg(cmd_buffer->cs, R_02823C_CB_SHADER_MASK, 0xf);

	for (unsigned i = 0; i < 32; ++i) {
		unsigned vs_offset, ps_offset, flat_shade;
		if (!(ps->info.fs.input_mask & (1u << i)))
			continue;

		assert(vs->info.vs.export_mask & (1u << i));

		ps_offset = util_bitcount(ps->info.fs.input_mask & ((1u << i) - 1));
		vs_offset = util_bitcount(vs->info.vs.export_mask & ((1u << i) - 1));
		flat_shade = !!(ps->info.fs.flat_shaded_mask & (1u << ps_offset));
		radeon_set_context_reg(cmd_buffer->cs, R_028644_SPI_PS_INPUT_CNTL_0 + 4 * ps_offset,
				       S_028644_OFFSET(vs_offset) | S_028644_FLAT_SHADE(flat_shade));
	}
}

static void
radv_emit_graphics_pipeline(struct radv_cmd_buffer *cmd_buffer,
                            struct radv_pipeline *pipeline)
{
	radv_emit_graphics_depth_stencil_state(cmd_buffer, pipeline);
	radv_emit_graphics_blend_state(cmd_buffer, pipeline);
	radv_emit_graphics_raster_state(cmd_buffer, pipeline);

	radv_emit_vertex_shader(cmd_buffer, pipeline);
	radv_emit_fragment_shader(cmd_buffer, pipeline);
}

static void
radv_emit_viewport(struct radv_cmd_buffer *cmd_buffer)
{
	si_write_viewport(cmd_buffer->cs, 0, cmd_buffer->state.dynamic.viewport.count,
			  cmd_buffer->state.dynamic.viewport.viewports);
}

static void
radv_emit_scissor(struct radv_cmd_buffer *cmd_buffer)
{
	uint32_t count = cmd_buffer->state.dynamic.scissor.count;
	si_write_scissors(cmd_buffer->cs, 0, count,
			  cmd_buffer->state.dynamic.scissor.scissors);
	radeon_set_context_reg(cmd_buffer->cs, R_028A48_PA_SC_MODE_CNTL_0,
			       cmd_buffer->state.pipeline->graphics.raster.pa_sc_mode_cntl_0 | S_028A48_VPORT_SCISSOR_ENABLE(count ? 1 : 0));
}

static void
radv_emit_fb_color_state(struct radv_cmd_buffer *cmd_buffer,
			 struct radv_color_buffer_info *cb)
{
	bool is_vi = cmd_buffer->device->instance->physicalDevice.rad_info.chip_class >= VI;
	radeon_set_context_reg_seq(cmd_buffer->cs, R_028C60_CB_COLOR0_BASE + cb->color_index * 0x3c, is_vi ? 14 : 13);
	radeon_emit(cmd_buffer->cs, cb->cb_color_base);
	radeon_emit(cmd_buffer->cs, cb->cb_color_pitch);
	radeon_emit(cmd_buffer->cs, cb->cb_color_slice);
	radeon_emit(cmd_buffer->cs, cb->cb_color_view);
	radeon_emit(cmd_buffer->cs, cb->cb_color_info);
	radeon_emit(cmd_buffer->cs, cb->cb_color_attrib);
	radeon_emit(cmd_buffer->cs, cb->cb_dcc_control);
	radeon_emit(cmd_buffer->cs, cb->cb_color_cmask);
	radeon_emit(cmd_buffer->cs, cb->cb_color_cmask_slice);
	radeon_emit(cmd_buffer->cs, cb->cb_color_fmask);
	radeon_emit(cmd_buffer->cs, cb->cb_color_fmask_slice);
	radeon_emit(cmd_buffer->cs, cb->cb_clear_value0);
	radeon_emit(cmd_buffer->cs, cb->cb_clear_value1);
	if (is_vi) { /* DCC BASE */
		radeon_emit(cmd_buffer->cs, 0);
	}
}

static void
radv_emit_fb_ds_state(struct radv_cmd_buffer *cmd_buffer,
		      struct radv_ds_buffer_info *ds)
{
	radeon_set_context_reg(cmd_buffer->cs, R_028008_DB_DEPTH_VIEW, ds->db_depth_view);
	radeon_set_context_reg(cmd_buffer->cs, R_028014_DB_HTILE_DATA_BASE, ds->db_htile_data_base);

	radeon_set_context_reg_seq(cmd_buffer->cs, R_02803C_DB_DEPTH_INFO, 9);
	radeon_emit(cmd_buffer->cs, ds->db_depth_info);	/* R_02803C_DB_DEPTH_INFO */
	radeon_emit(cmd_buffer->cs, ds->db_z_info |		/* R_028040_DB_Z_INFO */
		    S_028040_ZRANGE_PRECISION(1));
	radeon_emit(cmd_buffer->cs, ds->db_stencil_info);	/* R_028044_DB_STENCIL_INFO */
	radeon_emit(cmd_buffer->cs, ds->db_z_read_base);	/* R_028048_DB_Z_READ_BASE */
	radeon_emit(cmd_buffer->cs, ds->db_stencil_read_base);	/* R_02804C_DB_STENCIL_READ_BASE */
	radeon_emit(cmd_buffer->cs, ds->db_z_write_base);	/* R_028050_DB_Z_WRITE_BASE */
	radeon_emit(cmd_buffer->cs, ds->db_stencil_write_base);	/* R_028054_DB_STENCIL_WRITE_BASE */
	radeon_emit(cmd_buffer->cs, ds->db_depth_size);	/* R_028058_DB_DEPTH_SIZE */
	radeon_emit(cmd_buffer->cs, ds->db_depth_slice);	/* R_02805C_DB_DEPTH_SLICE */

	radeon_set_context_reg_seq(cmd_buffer->cs, R_028028_DB_STENCIL_CLEAR, 2);
	radeon_emit(cmd_buffer->cs, ds->db_stencil_clear); /* R_028028_DB_STENCIL_CLEAR */
	radeon_emit(cmd_buffer->cs, ds->db_depth_clear); /* R_02802C_DB_DEPTH_CLEAR */

	radeon_set_context_reg(cmd_buffer->cs, R_028ABC_DB_HTILE_SURFACE, ds->db_htile_surface);
	radeon_set_context_reg(cmd_buffer->cs, R_028B78_PA_SU_POLY_OFFSET_DB_FMT_CNTL,
			       ds->pa_su_poly_offset_db_fmt_cntl);
}

static void
radv_emit_framebuffer_state(struct radv_cmd_buffer *cmd_buffer)
{
	int i;
	struct radv_framebuffer *framebuffer = cmd_buffer->state.framebuffer;
	int color_count = 0;
	bool has_ds = false;
	for (i = 0; i < framebuffer->attachment_count; i++) {
		struct radv_attachment_info *att = &framebuffer->attachments[i];

		cmd_buffer->device->ws->cs_add_buffer(cmd_buffer->cs, att->attachment->bo->bo, 8);

		if (att->attachment->aspect_mask & VK_IMAGE_ASPECT_COLOR_BIT) {
			color_count++;
			radv_emit_fb_color_state(cmd_buffer, &att->cb);
		} else {
			radv_emit_fb_ds_state(cmd_buffer, &att->ds);
			has_ds = true;
		}
	}

	for (i = color_count; i < 8; i++)
		radeon_set_context_reg(cmd_buffer->cs, R_028C70_CB_COLOR0_INFO + i * 0x3C,
				       S_028C70_FORMAT(V_028C70_COLOR_INVALID));

	if (!has_ds) {
		radeon_set_context_reg_seq(cmd_buffer->cs, R_028040_DB_Z_INFO, 2);
		radeon_emit(cmd_buffer->cs, S_028040_FORMAT(V_028040_Z_INVALID)); /* R_028040_DB_Z_INFO */
		radeon_emit(cmd_buffer->cs, S_028044_FORMAT(V_028044_STENCIL_INVALID)); /* R_028044_DB_STENCIL_INFO */
	}
	radeon_set_context_reg(cmd_buffer->cs, R_028208_PA_SC_WINDOW_SCISSOR_BR,
			       S_028208_BR_X(framebuffer->width) |
			       S_028208_BR_Y(framebuffer->height));
}

static void
radv_cmd_buffer_flush_dynamic_state(struct radv_cmd_buffer *cmd_buffer)
{

	if (cmd_buffer->state.dirty ) {
		unsigned width = cmd_buffer->state.dynamic.line_width * 8;
		radeon_set_context_reg(cmd_buffer->cs, R_028A08_PA_SU_LINE_CNTL,
				       S_028A08_WIDTH(CLAMP(width, 0, 0xFFF)));
	}

	if (cmd_buffer->state.dirty & (RADV_CMD_DIRTY_DYNAMIC_STENCIL_REFERENCE |
				       RADV_CMD_DIRTY_DYNAMIC_STENCIL_WRITE_MASK |
				       RADV_CMD_DIRTY_DYNAMIC_STENCIL_COMPARE_MASK)) {
		struct radv_dynamic_state *d = &cmd_buffer->state.dynamic;
		radeon_set_context_reg_seq(cmd_buffer->cs, R_028430_DB_STENCILREFMASK, 2);
		radeon_emit(cmd_buffer->cs, S_028430_STENCILTESTVAL(d->stencil_reference.front) |
			    S_028430_STENCILMASK(d->stencil_compare_mask.front) |
			    S_028430_STENCILWRITEMASK(d->stencil_write_mask.front) |
			    S_028430_STENCILOPVAL(1));
		radeon_emit(cmd_buffer->cs, S_028434_STENCILTESTVAL_BF(d->stencil_reference.back) |
			    S_028434_STENCILMASK_BF(d->stencil_compare_mask.back) |
			    S_028434_STENCILWRITEMASK_BF(d->stencil_write_mask.back) |
			    S_028434_STENCILOPVAL_BF(1));
	}
	cmd_buffer->state.dirty = 0;
}

static void
radv_flush_constants(struct radv_cmd_buffer *cmd_buffer,
		     struct radv_pipeline_layout *layout,
		     VkShaderStageFlags stages) {
	unsigned offset;
	void *ptr;
	uint64_t va;

	stages &= cmd_buffer->push_constant_stages;
	if (!stages || !layout)
		return;

	radv_cmd_buffer_upload_alloc(cmd_buffer, layout->push_constant_size +
	                                         16 * layout->dynamic_offset_count,
				     256, &offset, &ptr);

	memcpy(ptr, cmd_buffer->push_constants, layout->push_constant_size);
	memcpy((char*)ptr + layout->push_constant_size, cmd_buffer->dynamic_buffers,
	       16 * layout->dynamic_offset_count);

	va = cmd_buffer->device->ws->buffer_get_va(cmd_buffer->upload.upload_bo.bo);
	va += offset;

	if (stages & VK_SHADER_STAGE_VERTEX_BIT) {
		radeon_set_sh_reg_seq(cmd_buffer->cs,
				      R_00B130_SPI_SHADER_USER_DATA_VS_0 + 8 * 4, 2);
		radeon_emit(cmd_buffer->cs, va);
		radeon_emit(cmd_buffer->cs, va >> 32);
	}

	if (stages & VK_SHADER_STAGE_FRAGMENT_BIT) {
		radeon_set_sh_reg_seq(cmd_buffer->cs,
				      R_00B030_SPI_SHADER_USER_DATA_PS_0 + 8 * 4, 2);
		radeon_emit(cmd_buffer->cs, va);
		radeon_emit(cmd_buffer->cs, va >> 32);
	}

	if (stages & VK_SHADER_STAGE_COMPUTE_BIT) {
		radeon_set_sh_reg_seq(cmd_buffer->cs,
				      R_00B900_COMPUTE_USER_DATA_0 + 8 * 4, 2);
		radeon_emit(cmd_buffer->cs, va);
		radeon_emit(cmd_buffer->cs, va >> 32);
	}

	cmd_buffer->push_constant_stages &= ~stages;
}

static void
radv_cmd_buffer_flush_state(struct radv_cmd_buffer *cmd_buffer)
{
	struct radv_pipeline *pipeline = cmd_buffer->state.pipeline;
	struct radv_device *device = cmd_buffer->device;
	uint32_t ia_multi_vgt_param;
	uint32_t ls_hs_config = 0;

	unsigned cdw_max = radeon_check_space(cmd_buffer->device->ws, cmd_buffer->cs,
					      4096);

	if (cmd_buffer->state.vertex_descriptors_dirty || cmd_buffer->state.vb_dirty) {
		unsigned vb_offset;
		void *vb_ptr;
		uint32_t ve, i = 0;
		uint32_t num_attribs = cmd_buffer->state.pipeline->num_vertex_attribs;
		uint64_t va;

		/* allocate some descriptor state for vertex buffers */
		radv_cmd_buffer_upload_alloc(cmd_buffer, num_attribs * 16, 256,
					     &vb_offset, &vb_ptr);

		for (i = 0; i < num_attribs; i++) {
			uint32_t *desc = &((uint32_t *)vb_ptr)[i * 4];
			uint32_t offset;
			int vb = cmd_buffer->state.pipeline->va_binding[i];
			struct radv_buffer *buffer = cmd_buffer->state.vertex_bindings[vb].buffer;
			uint32_t stride = cmd_buffer->state.pipeline->binding_stride[vb];

			device->ws->cs_add_buffer(cmd_buffer->cs, buffer->bo->bo, 8);
			va = device->ws->buffer_get_va(buffer->bo->bo);

			offset = cmd_buffer->state.vertex_bindings[vb].offset + buffer->offset + cmd_buffer->state.pipeline->va_offset[i];
			va += offset;
			desc[0] = va;
			desc[1] = S_008F04_BASE_ADDRESS_HI(va >> 32) | S_008F04_STRIDE(stride);
			desc[2] = buffer->size - offset;
			//TODO CIK
			desc[3] = cmd_buffer->state.pipeline->va_rsrc_word3[i];
		}

		va = device->ws->buffer_get_va(cmd_buffer->upload.upload_bo.bo);
		va += vb_offset;
		radeon_set_sh_reg_seq(cmd_buffer->cs,
				      R_00B130_SPI_SHADER_USER_DATA_VS_0 + 10 * 4, 2);
		radeon_emit(cmd_buffer->cs, va);
		radeon_emit(cmd_buffer->cs, va >> 32);

	}

	cmd_buffer->state.vertex_descriptors_dirty = false;
	cmd_buffer->state.vb_dirty = 0;
	if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_PIPELINE)
		radv_emit_graphics_pipeline(cmd_buffer, pipeline);

	if (cmd_buffer->state.dirty & (RADV_CMD_DIRTY_DYNAMIC_VIEWPORT |
				       RADV_CMD_DIRTY_PIPELINE))
		radv_emit_viewport(cmd_buffer);

	if (cmd_buffer->state.dirty & (RADV_CMD_DIRTY_DYNAMIC_SCISSOR |
				       RADV_CMD_DIRTY_PIPELINE))
		radv_emit_scissor(cmd_buffer);

	if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_INDEX_BUFFER) {
		radeon_emit(cmd_buffer->cs, PKT3(PKT3_INDEX_TYPE, 0, 0));
		radeon_emit(cmd_buffer->cs, cmd_buffer->state.index_type);
	}

	if (cmd_buffer->state.dirty & RADV_CMD_DIRTY_PIPELINE) {
		radeon_set_context_reg(cmd_buffer->cs, R_028B54_VGT_SHADER_STAGES_EN, 0);
		ia_multi_vgt_param = si_get_ia_multi_vgt_param(cmd_buffer);
		/* TODO CIK only */
		radeon_emit(cmd_buffer->cs, PKT3(PKT3_DRAW_PREAMBLE, 2, 0));
		radeon_emit(cmd_buffer->cs, cmd_buffer->state.pipeline->graphics.prim); /* VGT_PRIMITIVE_TYPE */
		radeon_emit(cmd_buffer->cs, ia_multi_vgt_param); /* IA_MULTI_VGT_PARAM */
		radeon_emit(cmd_buffer->cs, ls_hs_config); /* VGT_LS_HS_CONFIG */

		radeon_set_context_reg(cmd_buffer->cs, R_028A6C_VGT_GS_OUT_PRIM_TYPE, 2);
	}

	radv_cmd_buffer_flush_dynamic_state(cmd_buffer);

	radv_flush_constants(cmd_buffer, cmd_buffer->state.pipeline->layout,
			     VK_SHADER_STAGE_ALL_GRAPHICS);

	assert(cmd_buffer->cs->cdw <= cdw_max);
}

static void
radv_cmd_buffer_set_subpass(struct radv_cmd_buffer *cmd_buffer,
                            struct radv_subpass *subpass)
{
	cmd_buffer->state.subpass = subpass;
}

static void
radv_cmd_state_setup_attachments(struct radv_cmd_buffer *cmd_buffer,
                                 const VkRenderPassBeginInfo *info)
{
	struct radv_cmd_state *state = &cmd_buffer->state;
	RADV_FROM_HANDLE(radv_render_pass, pass, info->renderPass);

	radv_free(&cmd_buffer->pool->alloc, state->attachments);

	if (pass->attachment_count == 0) {
		state->attachments = NULL;
		return;
	}

	state->attachments = radv_alloc(&cmd_buffer->pool->alloc,
					pass->attachment_count *
					sizeof(state->attachments[0]),
					8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (state->attachments == NULL) {
		/* FIXME: Propagate VK_ERROR_OUT_OF_HOST_MEMORY to vkEndCommandBuffer */
		abort();
	}

	for (uint32_t i = 0; i < pass->attachment_count; ++i) {
		struct radv_render_pass_attachment *att = &pass->attachments[i];
		VkImageAspectFlags att_aspects = vk_format_aspects(att->format);
		VkImageAspectFlags clear_aspects = 0;

		if (att_aspects == VK_IMAGE_ASPECT_COLOR_BIT) {
			/* color attachment */
			if (att->load_op == VK_ATTACHMENT_LOAD_OP_CLEAR) {
				clear_aspects |= VK_IMAGE_ASPECT_COLOR_BIT;
			}
		} else {
			/* depthstencil attachment */
			if ((att_aspects & VK_IMAGE_ASPECT_DEPTH_BIT) &&
			    att->load_op == VK_ATTACHMENT_LOAD_OP_CLEAR) {
				clear_aspects |= VK_IMAGE_ASPECT_DEPTH_BIT;
			}
			if ((att_aspects & VK_IMAGE_ASPECT_STENCIL_BIT) &&
			    att->stencil_load_op == VK_ATTACHMENT_LOAD_OP_CLEAR) {
				clear_aspects |= VK_IMAGE_ASPECT_STENCIL_BIT;
			}
		}

		state->attachments[i].pending_clear_aspects = clear_aspects;
		if (clear_aspects) {
			assert(info->clearValueCount > i);
			state->attachments[i].clear_value = info->pClearValues[i];
		}
	}
}

VkResult radv_AllocateCommandBuffers(
	VkDevice                                    _device,
	const VkCommandBufferAllocateInfo*          pAllocateInfo,
	VkCommandBuffer*                            pCommandBuffers)
{
	RADV_FROM_HANDLE(radv_device, device, _device);
	RADV_FROM_HANDLE(radv_cmd_pool, pool, pAllocateInfo->commandPool);

	VkResult result = VK_SUCCESS;
	uint32_t i;

	for (i = 0; i < pAllocateInfo->commandBufferCount; i++) {
		result = radv_create_cmd_buffer(device, pool, pAllocateInfo->level,
						&pCommandBuffers[i]);
		if (result != VK_SUCCESS)
			break;
	}

	if (result != VK_SUCCESS)
		radv_FreeCommandBuffers(_device, pAllocateInfo->commandPool,
					i, pCommandBuffers);

	return result;
}

static void
radv_cmd_buffer_destroy(struct radv_cmd_buffer *cmd_buffer)
{
	list_del(&cmd_buffer->pool_link);

	cmd_buffer->device->ws->buffer_destroy(cmd_buffer->upload.upload_bo.bo);
	cmd_buffer->device->ws->buffer_destroy(cmd_buffer->border_color_bo.bo);
	cmd_buffer->device->ws->cs_destroy(cmd_buffer->cs);
	radv_free(&cmd_buffer->pool->alloc, cmd_buffer);
}

void radv_FreeCommandBuffers(
	VkDevice                                    device,
	VkCommandPool                               commandPool,
	uint32_t                                    commandBufferCount,
	const VkCommandBuffer*                      pCommandBuffers)
{
	for (uint32_t i = 0; i < commandBufferCount; i++) {
		RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, pCommandBuffers[i]);

		radv_cmd_buffer_destroy(cmd_buffer);
	}
}

static void  radv_reset_cmd_buffer(struct radv_cmd_buffer *cmd_buffer)
{
	cmd_buffer->device->ws->cs_reset(cmd_buffer->cs);
	cmd_buffer->upload.offset = 0;
}

VkResult radv_ResetCommandBuffer(
	VkCommandBuffer                             commandBuffer,
	VkCommandBufferResetFlags                   flags)
{
	RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
	radv_reset_cmd_buffer(cmd_buffer);
	return VK_SUCCESS;
}

VkResult radv_BeginCommandBuffer(
	VkCommandBuffer                             commandBuffer,
	const VkCommandBufferBeginInfo*             pBeginInfo)
{
	RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
	radv_reset_cmd_buffer(cmd_buffer);

	/* setup initial configuration into command buffer */
	si_init_config(&cmd_buffer->device->instance->physicalDevice, cmd_buffer);
	return VK_SUCCESS;
}

void radv_CmdBindVertexBuffers(
	VkCommandBuffer                             commandBuffer,
	uint32_t                                    firstBinding,
	uint32_t                                    bindingCount,
	const VkBuffer*                             pBuffers,
	const VkDeviceSize*                         pOffsets)
{
	RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
	struct radv_vertex_binding *vb = cmd_buffer->state.vertex_bindings;

	/* We have to defer setting up vertex buffer since we need the buffer
	 * stride from the pipeline. */

	assert(firstBinding + bindingCount < MAX_VBS);
	for (uint32_t i = 0; i < bindingCount; i++) {
		vb[firstBinding + i].buffer = radv_buffer_from_handle(pBuffers[i]);
		vb[firstBinding + i].offset = pOffsets[i];
		cmd_buffer->state.vb_dirty |= 1 << (firstBinding + i);
	}
}

void radv_CmdBindIndexBuffer(
	VkCommandBuffer                             commandBuffer,
	VkBuffer buffer,
	VkDeviceSize offset,
	VkIndexType indexType)
{
	RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);

	cmd_buffer->state.index_buffer = radv_buffer_from_handle(buffer);
	cmd_buffer->state.index_offset = offset;
	cmd_buffer->state.index_type = indexType; /* vk matches hw */
	cmd_buffer->state.dirty |= RADV_CMD_DIRTY_INDEX_BUFFER;
	cmd_buffer->device->ws->cs_add_buffer(cmd_buffer->cs, cmd_buffer->state.index_buffer->bo->bo, 8);
}

void radv_CmdBindDescriptorSets(
	VkCommandBuffer                             commandBuffer,
	VkPipelineBindPoint                         pipelineBindPoint,
	VkPipelineLayout                            _layout,
	uint32_t                                    firstSet,
	uint32_t                                    descriptorSetCount,
	const VkDescriptorSet*                      pDescriptorSets,
	uint32_t                                    dynamicOffsetCount,
	const uint32_t*                             pDynamicOffsets)
{
	RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
	RADV_FROM_HANDLE(radv_pipeline_layout, layout, _layout);
	struct radeon_winsys *ws = cmd_buffer->device->ws;
	unsigned dyn_idx = 0;

	unsigned cdw_max = radeon_check_space(cmd_buffer->device->ws, cmd_buffer->cs,
					      MAX_SETS * 4 * 6);

	for (unsigned i = 0; i < descriptorSetCount; ++i) {
		unsigned idx = i + firstSet;
		RADV_FROM_HANDLE(radv_descriptor_set, set, pDescriptorSets[i]);
		uint64_t va;

		va = set->bo.bo ? ws->buffer_get_va(set->bo.bo) : 0;

		for (unsigned j = 0; j < set->layout->buffer_count; ++j)
			if (set->descriptors[j])
				ws->cs_add_buffer(cmd_buffer->cs, set->descriptors[j]->bo, 7);

		radeon_set_sh_reg_seq(cmd_buffer->cs,
				      R_00B030_SPI_SHADER_USER_DATA_PS_0 + 8 * idx, 2);
		radeon_emit(cmd_buffer->cs, va);
		radeon_emit(cmd_buffer->cs, va >> 32);

		radeon_set_sh_reg_seq(cmd_buffer->cs,
				      R_00B130_SPI_SHADER_USER_DATA_VS_0 + 8 * idx, 2);
		radeon_emit(cmd_buffer->cs, va);
		radeon_emit(cmd_buffer->cs, va >> 32);

		radeon_set_sh_reg_seq(cmd_buffer->cs,
				      R_00B900_COMPUTE_USER_DATA_0 + 8 * idx, 2);
		radeon_emit(cmd_buffer->cs, va);
		radeon_emit(cmd_buffer->cs, va >> 32);

		if(set->bo.bo)
			ws->cs_add_buffer(cmd_buffer->cs, set->bo.bo, 8);

		for(unsigned j = 0; j < set->layout->dynamic_offset_count; ++j, ++dyn_idx) {
			unsigned idx = j + layout->set[i].dynamic_offset_start;
			uint32_t *dst = cmd_buffer->dynamic_buffers + idx * 4;
			assert(dyn_idx < dynamicOffsetCount);

			struct radv_descriptor_range *range = set->dynamic_descriptors + idx;
			uint64_t va = range->va + pDynamicOffsets[dyn_idx];
			dst[0] = va;
			dst[1] = S_008F04_BASE_ADDRESS_HI(va >> 32);
			dst[2] = range->size;
			dst[3] = S_008F0C_DST_SEL_X(V_008F0C_SQ_SEL_X) |
			         S_008F0C_DST_SEL_Y(V_008F0C_SQ_SEL_Y) |
			         S_008F0C_DST_SEL_Z(V_008F0C_SQ_SEL_Z) |
			         S_008F0C_DST_SEL_W(V_008F0C_SQ_SEL_W) |
			         S_008F0C_NUM_FORMAT(V_008F0C_BUF_NUM_FORMAT_FLOAT) |
			         S_008F0C_DATA_FORMAT(V_008F0C_BUF_DATA_FORMAT_32);
			cmd_buffer->push_constant_stages |=
			                     set->layout->dynamic_shader_stages;
		}
	}

	assert(cmd_buffer->cs->cdw <= cdw_max);
}

void radv_CmdPushConstants(VkCommandBuffer commandBuffer,
			   VkPipelineLayout layout,
			   VkShaderStageFlags stageFlags,
			   uint32_t offset,
			   uint32_t size,
			   const void* pValues)
{
	RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
	memcpy(cmd_buffer->push_constants + offset, pValues, size);
	cmd_buffer->push_constant_stages |= stageFlags;
}

VkResult radv_EndCommandBuffer(
	VkCommandBuffer                             commandBuffer)
{
	RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
	if (!cmd_buffer->device->ws->cs_finalize(cmd_buffer->cs))
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;
	return VK_SUCCESS;
}

static void
radv_bind_compute_pipeline(struct radv_cmd_buffer *cmd_buffer,
                           struct radv_pipeline *pipeline)
{
	struct radeon_winsys *ws = cmd_buffer->device->ws;
	struct radv_shader_variant *compute_shader = pipeline->shaders[MESA_SHADER_COMPUTE];
	uint64_t va = ws->buffer_get_va(compute_shader->bo);

	ws->cs_add_buffer(cmd_buffer->cs, compute_shader->bo, 8);

	unsigned cdw_max = radeon_check_space(cmd_buffer->device->ws, cmd_buffer->cs, 16);

	radeon_set_sh_reg_seq(cmd_buffer->cs, R_00B830_COMPUTE_PGM_LO, 2);
	radeon_emit(cmd_buffer->cs, va >> 8);
	radeon_emit(cmd_buffer->cs, va >> 40);

	radeon_set_sh_reg_seq(cmd_buffer->cs, R_00B848_COMPUTE_PGM_RSRC1, 2);
	radeon_emit(cmd_buffer->cs, compute_shader->rsrc1);
	radeon_emit(cmd_buffer->cs, compute_shader->rsrc2);

	/* change these once we have scratch support */
	radeon_set_sh_reg(cmd_buffer->cs, R_00B860_COMPUTE_TMPRING_SIZE,
			  S_00B860_WAVES(32) | S_00B860_WAVESIZE(0));

	radeon_set_sh_reg_seq(cmd_buffer->cs, R_00B81C_COMPUTE_NUM_THREAD_X, 3);
	radeon_emit(cmd_buffer->cs,
		    S_00B81C_NUM_THREAD_FULL(pipeline->compute.block_size[0]));
	radeon_emit(cmd_buffer->cs,
		    S_00B81C_NUM_THREAD_FULL(pipeline->compute.block_size[1]));
	radeon_emit(cmd_buffer->cs,
		    S_00B81C_NUM_THREAD_FULL(pipeline->compute.block_size[2]));

	assert(cmd_buffer->cs->cdw <= cdw_max);
}


void radv_CmdBindPipeline(
	VkCommandBuffer                             commandBuffer,
	VkPipelineBindPoint                         pipelineBindPoint,
	VkPipeline                                  _pipeline)
{
	RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
	RADV_FROM_HANDLE(radv_pipeline, pipeline, _pipeline);

	switch (pipelineBindPoint) {
	case VK_PIPELINE_BIND_POINT_COMPUTE:
		cmd_buffer->state.compute_pipeline = pipeline;
		cmd_buffer->state.compute_dirty |= RADV_CMD_DIRTY_PIPELINE;
		cmd_buffer->state.descriptors_dirty |= VK_SHADER_STAGE_COMPUTE_BIT;
		cmd_buffer->push_constant_stages |= VK_SHADER_STAGE_COMPUTE_BIT;
		radv_bind_compute_pipeline(cmd_buffer, pipeline); // TODO remove
		break;
	case VK_PIPELINE_BIND_POINT_GRAPHICS:
		cmd_buffer->state.pipeline = pipeline;
		cmd_buffer->state.vertex_descriptors_dirty = true;
		cmd_buffer->state.dirty |= RADV_CMD_DIRTY_PIPELINE;
		cmd_buffer->state.descriptors_dirty |= pipeline->active_stages;
		cmd_buffer->push_constant_stages |= pipeline->active_stages;

		/* Apply the dynamic state from the pipeline */
		cmd_buffer->state.dirty |= pipeline->dynamic_state_mask;
		radv_dynamic_state_copy(&cmd_buffer->state.dynamic,
					&pipeline->dynamic_state,
					pipeline->dynamic_state_mask);
		break;
	default:
		assert(!"invalid bind point");
		break;
	}
}

void radv_CmdSetViewport(
	VkCommandBuffer                             commandBuffer,
	uint32_t                                    firstViewport,
	uint32_t                                    viewportCount,
	const VkViewport*                           pViewports)
{
	RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);

	const uint32_t total_count = firstViewport + viewportCount;
	if (cmd_buffer->state.dynamic.viewport.count < total_count)
		cmd_buffer->state.dynamic.viewport.count = total_count;

	memcpy(cmd_buffer->state.dynamic.viewport.viewports + firstViewport,
	       pViewports, viewportCount * sizeof(*pViewports));

	cmd_buffer->state.dirty |= RADV_CMD_DIRTY_DYNAMIC_VIEWPORT;
}

void radv_CmdSetScissor(
	VkCommandBuffer                             commandBuffer,
	uint32_t                                    firstScissor,
	uint32_t                                    scissorCount,
	const VkRect2D*                             pScissors)
{
	RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);

	const uint32_t total_count = firstScissor + scissorCount;
	if (cmd_buffer->state.dynamic.scissor.count < total_count)
		cmd_buffer->state.dynamic.scissor.count = total_count;

	memcpy(cmd_buffer->state.dynamic.scissor.scissors + firstScissor,
	       pScissors, scissorCount * sizeof(*pScissors));
	cmd_buffer->state.dirty |= RADV_CMD_DIRTY_DYNAMIC_SCISSOR;
}

void radv_CmdSetLineWidth(
	VkCommandBuffer                             commandBuffer,
	float                                       lineWidth)
{
	RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
	cmd_buffer->state.dynamic.line_width = lineWidth;
	cmd_buffer->state.dirty |= RADV_CMD_DIRTY_DYNAMIC_LINE_WIDTH;
}

void radv_CmdSetDepthBias(
	VkCommandBuffer                             commandBuffer,
	float                                       depthBiasConstantFactor,
	float                                       depthBiasClamp,
	float                                       depthBiasSlopeFactor)
{
	RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);

	cmd_buffer->state.dynamic.depth_bias.bias = depthBiasConstantFactor;
	cmd_buffer->state.dynamic.depth_bias.clamp = depthBiasClamp;
	cmd_buffer->state.dynamic.depth_bias.slope = depthBiasSlopeFactor;

	cmd_buffer->state.dirty |= RADV_CMD_DIRTY_DYNAMIC_DEPTH_BIAS;
}

void radv_CmdSetBlendConstants(
	VkCommandBuffer                             commandBuffer,
	const float                                 blendConstants[4])
{
	RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);

	memcpy(cmd_buffer->state.dynamic.blend_constants,
	       blendConstants, sizeof(float) * 4);

	cmd_buffer->state.dirty |= RADV_CMD_DIRTY_DYNAMIC_BLEND_CONSTANTS;
}

void radv_CmdSetDepthBounds(
	VkCommandBuffer                             commandBuffer,
	float                                       minDepthBounds,
	float                                       maxDepthBounds)
{
	RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);

	cmd_buffer->state.dynamic.depth_bounds.min = minDepthBounds;
	cmd_buffer->state.dynamic.depth_bounds.max = maxDepthBounds;

	cmd_buffer->state.dirty |= RADV_CMD_DIRTY_DYNAMIC_DEPTH_BOUNDS;
}

void radv_CmdSetStencilCompareMask(
	VkCommandBuffer                             commandBuffer,
	VkStencilFaceFlags                          faceMask,
	uint32_t                                    compareMask)
{
	RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);

	if (faceMask & VK_STENCIL_FACE_FRONT_BIT)
		cmd_buffer->state.dynamic.stencil_compare_mask.front = compareMask;
	if (faceMask & VK_STENCIL_FACE_BACK_BIT)
		cmd_buffer->state.dynamic.stencil_compare_mask.back = compareMask;

	cmd_buffer->state.dirty |= RADV_CMD_DIRTY_DYNAMIC_STENCIL_COMPARE_MASK;
}

void radv_CmdSetStencilWriteMask(
	VkCommandBuffer                             commandBuffer,
	VkStencilFaceFlags                          faceMask,
	uint32_t                                    writeMask)
{
	RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);

	if (faceMask & VK_STENCIL_FACE_FRONT_BIT)
		cmd_buffer->state.dynamic.stencil_write_mask.front = writeMask;
	if (faceMask & VK_STENCIL_FACE_BACK_BIT)
		cmd_buffer->state.dynamic.stencil_write_mask.back = writeMask;

	cmd_buffer->state.dirty |= RADV_CMD_DIRTY_DYNAMIC_STENCIL_WRITE_MASK;
}

void radv_CmdSetStencilReference(
	VkCommandBuffer                             commandBuffer,
	VkStencilFaceFlags                          faceMask,
	uint32_t                                    reference)
{
	RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);

	if (faceMask & VK_STENCIL_FACE_FRONT_BIT)
		cmd_buffer->state.dynamic.stencil_reference.front = reference;
	if (faceMask & VK_STENCIL_FACE_BACK_BIT)
		cmd_buffer->state.dynamic.stencil_reference.back = reference;

	cmd_buffer->state.dirty |= RADV_CMD_DIRTY_DYNAMIC_STENCIL_REFERENCE;
}


void radv_CmdExecuteCommands(
	VkCommandBuffer                             commandBuffer,
	uint32_t                                    commandBufferCount,
	const VkCommandBuffer*                      pCmdBuffers)
{
	//   RADV_FROM_HANDLE(radv_cmd_buffer, primary, commandBuffer);

	//   assert(primary->level == VK_COMMAND_BUFFER_LEVEL_PRIMARY);

	for (uint32_t i = 0; i < commandBufferCount; i++) {
		//      RADV_FROM_HANDLE(radv_cmd_buffer, secondary, pCmdBuffers[i]);

		//      assert(secondary->level == VK_COMMAND_BUFFER_LEVEL_SECONDARY);

		//.     radv_cmd_buffer_add_secondary(primary, secondary);
	}
}
VkResult radv_CreateCommandPool(
	VkDevice                                    _device,
	const VkCommandPoolCreateInfo*              pCreateInfo,
	const VkAllocationCallbacks*                pAllocator,
	VkCommandPool*                              pCmdPool)
{
	RADV_FROM_HANDLE(radv_device, device, _device);
	struct radv_cmd_pool *pool;

	pool = radv_alloc2(&device->alloc, pAllocator, sizeof(*pool), 8,
			   VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (pool == NULL)
		return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

	if (pAllocator)
		pool->alloc = *pAllocator;
	else
		pool->alloc = device->alloc;

	list_inithead(&pool->cmd_buffers);

	*pCmdPool = radv_cmd_pool_to_handle(pool);

	return VK_SUCCESS;

}

void radv_DestroyCommandPool(
	VkDevice                                    _device,
	VkCommandPool                               commandPool,
	const VkAllocationCallbacks*                pAllocator)
{
	RADV_FROM_HANDLE(radv_device, device, _device);
	RADV_FROM_HANDLE(radv_cmd_pool, pool, commandPool);

	list_for_each_entry_safe(struct radv_cmd_buffer, cmd_buffer,
				 &pool->cmd_buffers, pool_link) {
		//      radv_cmd_buffer_destroy(cmd_buffer);
	}

	radv_free2(&device->alloc, pAllocator, pool);
}

VkResult radv_ResetCommandPool(
	VkDevice                                    device,
	VkCommandPool                               commandPool,
	VkCommandPoolResetFlags                     flags)
{
	RADV_FROM_HANDLE(radv_cmd_pool, pool, commandPool);

	list_for_each_entry(struct radv_cmd_buffer, cmd_buffer,
			    &pool->cmd_buffers, pool_link) {
		//      radv_cmd_buffer_reset(cmd_buffer);
	}

	return VK_SUCCESS;
}

void radv_CmdBeginRenderPass(
	VkCommandBuffer                             commandBuffer,
	const VkRenderPassBeginInfo*                pRenderPassBegin,
	VkSubpassContents                           contents)
{
	RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
	RADV_FROM_HANDLE(radv_render_pass, pass, pRenderPassBegin->renderPass);
	RADV_FROM_HANDLE(radv_framebuffer, framebuffer, pRenderPassBegin->framebuffer);

	unsigned cdw_max = radeon_check_space(cmd_buffer->device->ws, cmd_buffer->cs,
					      2048);

	cmd_buffer->state.framebuffer = framebuffer;
	cmd_buffer->state.pass = pass;
	cmd_buffer->state.render_area = pRenderPassBegin->renderArea;
	radv_cmd_state_setup_attachments(cmd_buffer, pRenderPassBegin);

	radv_emit_framebuffer_state(cmd_buffer);
	radv_cmd_buffer_set_subpass(cmd_buffer, pass->subpasses);
	radv_cmd_buffer_clear_subpass(cmd_buffer);

	assert(cmd_buffer->cs->cdw <= cdw_max);
}

void radv_CmdDraw(
	VkCommandBuffer                             commandBuffer,
	uint32_t                                    vertexCount,
	uint32_t                                    instanceCount,
	uint32_t                                    firstVertex,
	uint32_t                                    firstInstance)
{
	RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
	radv_cmd_buffer_flush_state(cmd_buffer);

	unsigned cdw_max = radeon_check_space(cmd_buffer->device->ws, cmd_buffer->cs, 9);

	radeon_set_sh_reg_seq(cmd_buffer->cs, R_00B130_SPI_SHADER_USER_DATA_VS_0 + 12 * 4, 2);
	radeon_emit(cmd_buffer->cs, firstVertex);
	radeon_emit(cmd_buffer->cs, firstInstance);
	radeon_emit(cmd_buffer->cs, PKT3(PKT3_NUM_INSTANCES, 0, 0));
	radeon_emit(cmd_buffer->cs, instanceCount);

	radeon_emit(cmd_buffer->cs, PKT3(PKT3_DRAW_INDEX_AUTO, 1, 0));
	radeon_emit(cmd_buffer->cs, vertexCount);
	radeon_emit(cmd_buffer->cs, V_0287F0_DI_SRC_SEL_AUTO_INDEX |
		    S_0287F0_USE_OPAQUE(0));//!!info->count_from_stream_output));

	assert(cmd_buffer->cs->cdw <= cdw_max);
}

void radv_CmdDrawIndexed(
	VkCommandBuffer                             commandBuffer,
	uint32_t                                    indexCount,
	uint32_t                                    instanceCount,
	uint32_t                                    firstIndex,
	int32_t                                     vertexOffset,
	uint32_t                                    firstInstance)
{
	RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
	int index_size = cmd_buffer->state.index_type ? 2 : 1;
	uint32_t index_max_size = (cmd_buffer->state.index_buffer->size - cmd_buffer->state.index_buffer->offset) / index_size;
	uint64_t index_va;

	radv_cmd_buffer_flush_state(cmd_buffer);

	unsigned cdw_max = radeon_check_space(cmd_buffer->device->ws, cmd_buffer->cs, 12);

	radeon_set_sh_reg_seq(cmd_buffer->cs, R_00B130_SPI_SHADER_USER_DATA_VS_0 + 12 * 4, 2);
	radeon_emit(cmd_buffer->cs, vertexOffset);
	radeon_emit(cmd_buffer->cs, firstInstance);
	radeon_emit(cmd_buffer->cs, PKT3(PKT3_NUM_INSTANCES, 0, 0));
	radeon_emit(cmd_buffer->cs, instanceCount);

	index_va = cmd_buffer->device->ws->buffer_get_va(cmd_buffer->state.index_buffer->bo->bo);
	index_va += firstIndex * index_size;
	radeon_emit(cmd_buffer->cs, PKT3(PKT3_DRAW_INDEX_2, 4, false));
	radeon_emit(cmd_buffer->cs, index_max_size);
	radeon_emit(cmd_buffer->cs, index_va);
	radeon_emit(cmd_buffer->cs, (index_va >> 32UL) & 0xFF);
	radeon_emit(cmd_buffer->cs, indexCount);
	radeon_emit(cmd_buffer->cs, V_0287F0_DI_SRC_SEL_DMA);

	assert(cmd_buffer->cs->cdw <= cdw_max);
}

void radv_CmdDrawIndirect(
	VkCommandBuffer                             commandBuffer,
	VkBuffer                                    _buffer,
	VkDeviceSize                                offset,
	uint32_t                                    drawCount,
	uint32_t                                    stride)
{
	RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
	radv_cmd_buffer_flush_state(cmd_buffer);

}

void radv_CmdDrawIndexedIndirect(
	VkCommandBuffer                             commandBuffer,
	VkBuffer                                    _buffer,
	VkDeviceSize                                offset,
	uint32_t                                    drawCount,
	uint32_t                                    stride)
{
	RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
	radv_cmd_buffer_flush_state(cmd_buffer);

}

void radv_CmdDispatch(
	VkCommandBuffer                             commandBuffer,
	uint32_t                                    x,
	uint32_t                                    y,
	uint32_t                                    z)
{
	RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);

	radv_flush_constants(cmd_buffer, cmd_buffer->state.compute_pipeline->layout,
			     VK_SHADER_STAGE_COMPUTE_BIT);
	unsigned cdw_max = radeon_check_space(cmd_buffer->device->ws, cmd_buffer->cs, 10);

	radeon_set_sh_reg_seq(cmd_buffer->cs, R_00B900_COMPUTE_USER_DATA_0 + 10 * 4, 3);
	radeon_emit(cmd_buffer->cs, x);
	radeon_emit(cmd_buffer->cs, y);
	radeon_emit(cmd_buffer->cs, z);

	radeon_emit(cmd_buffer->cs, PKT3(PKT3_DISPATCH_DIRECT, 3, 0) |
		    PKT3_SHADER_TYPE_S(1));
	radeon_emit(cmd_buffer->cs, x);
	radeon_emit(cmd_buffer->cs, y);
	radeon_emit(cmd_buffer->cs, z);
	radeon_emit(cmd_buffer->cs, 1);

	assert(cmd_buffer->cs->cdw <= cdw_max);
}

void radv_CmdEndRenderPass(
	VkCommandBuffer                             commandBuffer)
{
	RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);

	si_emit_cache_flush(cmd_buffer);
	//   radv_cmd_buffer_resolve_subpass(cmd_buffer);
}

void radv_CmdPipelineBarrier(
	VkCommandBuffer                             commandBuffer,
	VkPipelineStageFlags                        srcStageMask,
	VkPipelineStageFlags                        destStageMask,
	VkBool32                                    byRegion,
	uint32_t                                    memoryBarrierCount,
	const VkMemoryBarrier*                      pMemoryBarriers,
	uint32_t                                    bufferMemoryBarrierCount,
	const VkBufferMemoryBarrier*                pBufferMemoryBarriers,
	uint32_t                                    imageMemoryBarrierCount,
	const VkImageMemoryBarrier*                 pImageMemoryBarriers)
{
	//   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);

}
