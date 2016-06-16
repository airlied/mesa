/*
 * Copyright © 2016 Red Hat
 *
 * based on anv driver:
 * Copyright © 2016 Intel Corporation
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

#include "radv_meta.h"
#include "nir/nir_builder.h"

enum blit2d_dst_type {
	/* We can bind this destination as a "normal" render target and render
	 * to it just like you would anywhere else.
	 */
	BLIT2D_DST_TYPE_NORMAL,

	/* The destination has a 3-channel RGB format.  Since we can't render to
	 * non-power-of-two textures, we have to bind it as a red texture and
	 * select the correct component for the given red pixel in the shader.
	 */
	BLIT2D_DST_TYPE_RGB,

	BLIT2D_NUM_DST_TYPES,
};

static VkFormat
vk_format_for_size(int bs)
{
	/* The choice of UNORM and UINT formats is very intentional here.  Most of
	 * the time, we want to use a UINT format to avoid any rounding error in
	 * the blit.  For stencil blits, R8_UINT is required by the hardware.
	 * (It's the only format allowed in conjunction with W-tiling.)  Also we
	 * intentionally use the 4-channel formats whenever we can.  This is so
	 * that, when we do a RGB <-> RGBX copy, the two formats will line up even
	 * though one of them is 3/4 the size of the other.  The choice of UNORM
	 * vs. UINT is also very intentional because Haswell doesn't handle 8 or
	 * 16-bit RGB UINT formats at all so we have to use UNORM there.
	 * Fortunately, the only time we should ever use two different formats in
	 * the table below is for RGB -> RGBA blits and so we will never have any
	 * UNORM/UINT mismatch.
	 */
	switch (bs) {
	case 1: return VK_FORMAT_R8_UINT;
	case 2: return VK_FORMAT_R8G8_UINT;
	case 3: return VK_FORMAT_R8G8B8_UNORM;
	case 4: return VK_FORMAT_R8G8B8A8_UNORM;
	case 6: return VK_FORMAT_R16G16B16_UNORM;
	case 8: return VK_FORMAT_R16G16B16A16_UINT;
	case 12: return VK_FORMAT_R32G32B32_UINT;
	case 16: return VK_FORMAT_R32G32B32A32_UINT;
	default:
		unreachable("Invalid format block size");
	}
}

static void
create_iview(struct radv_cmd_buffer *cmd_buffer,
             struct radv_meta_blit2d_surf *surf,
             uint64_t offset,
             VkImageUsageFlags usage,
             uint32_t width,
             uint32_t height,
             VkImage *img,
             struct radv_image_view *iview)
{
	const VkImageCreateInfo image_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = vk_format_for_size(surf->bs),
		.extent = {
			.width = width,
			.height = height,
			.depth = 1,
		},
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = 1,
		.tiling = surf->tiling,
		.usage = usage,
	};

	/* Create the VkImage that is bound to the surface's memory. */
	radv_image_create(radv_device_to_handle(cmd_buffer->device),
			  &(struct radv_image_create_info) {
				  .vk_info = &image_info,
					  .stride = surf->pitch,
					  }, &cmd_buffer->pool->alloc, img);

	/* We could use a vk call to bind memory, but that would require
	 * creating a dummy memory object etc. so there's really no point.
	 */
	radv_image_from_handle(*img)->bo = surf->bo;
	radv_image_from_handle(*img)->offset = surf->base_offset + offset;

	radv_image_view_init(iview, cmd_buffer->device,
			     &(VkImageViewCreateInfo) {
				     .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
					     .image = *img,
					     .viewType = VK_IMAGE_VIEW_TYPE_2D,
					     .format = image_info.format,
					     .subresourceRange = {
					     .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
					     .baseMipLevel = 0,
					     .levelCount = 1,
					     .baseArrayLayer = 0,
					     .layerCount = 1
				     },
					     }, cmd_buffer, usage);
}

struct blit2d_src_temps {
	VkImage image;
	struct radv_image_view iview;

	VkDescriptorPool desc_pool;
	VkDescriptorSet set;
};

static void
blit2d_bind_src(struct radv_cmd_buffer *cmd_buffer,
                struct radv_meta_blit2d_surf *src,
                struct radv_meta_blit2d_rect *rect,
                struct blit2d_src_temps *tmp)
{
	struct radv_device *device = cmd_buffer->device;
	VkDevice vk_device = radv_device_to_handle(cmd_buffer->device);

	{
		uint32_t offset = 0;
		create_iview(cmd_buffer, src, offset, VK_IMAGE_USAGE_SAMPLED_BIT,
			     rect->src_x + rect->width, rect->src_y + rect->height,
			     &tmp->image, &tmp->iview);

		radv_CreateDescriptorPool(vk_device,
					  &(const VkDescriptorPoolCreateInfo) {
						  .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
							  .pNext = NULL,
							  .flags = 0,
							  .maxSets = 1,
							  .poolSizeCount = 1,
							  .pPoolSizes = (VkDescriptorPoolSize[]) {
							  {
								  .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
								  .descriptorCount = 1
							  },
						  }
					  }, &cmd_buffer->pool->alloc, &tmp->desc_pool);

		radv_AllocateDescriptorSets(vk_device,
					    &(VkDescriptorSetAllocateInfo) {
						    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
							    .descriptorPool = tmp->desc_pool,
							    .descriptorSetCount = 1,
							    .pSetLayouts = &device->meta_state.blit2d.img_ds_layout
							    }, &tmp->set);

		radv_UpdateDescriptorSets(vk_device,
					  1, /* writeCount */
					  (VkWriteDescriptorSet[]) {
						  {
							  .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
								  .dstSet = tmp->set,
								  .dstBinding = 0,
								  .dstArrayElement = 0,
								  .descriptorCount = 1,
								  .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
								  .pImageInfo = (VkDescriptorImageInfo[]) {
								  {
									  .sampler = NULL,
									  .imageView = radv_image_view_to_handle(&tmp->iview),
									  .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
								  },
							  }
						  }
					  }, 0, NULL);

		radv_CmdBindDescriptorSets(radv_cmd_buffer_to_handle(cmd_buffer),
					   VK_PIPELINE_BIND_POINT_GRAPHICS,
					   device->meta_state.blit2d.img_p_layout, 0, 1,
					   &tmp->set, 0, NULL);
	}
}

static void
blit2d_unbind_src(struct radv_cmd_buffer *cmd_buffer,
                  struct blit2d_src_temps *tmp)
{
	radv_DestroyDescriptorPool(radv_device_to_handle(cmd_buffer->device),
				   tmp->desc_pool, &cmd_buffer->pool->alloc);
	radv_DestroyImage(radv_device_to_handle(cmd_buffer->device),
			  tmp->image, &cmd_buffer->pool->alloc);
}

struct blit2d_dst_temps {
	VkImage image;
	struct radv_image_view iview;
	VkFramebuffer fb;
};

static void
blit2d_bind_dst(struct radv_cmd_buffer *cmd_buffer,
                struct radv_meta_blit2d_surf *dst,
                uint64_t offset,
                uint32_t width,
                uint32_t height,
                struct blit2d_dst_temps *tmp)
{
	create_iview(cmd_buffer, dst, offset, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
		     width, height, &tmp->image, &tmp->iview);

	radv_CreateFramebuffer(radv_device_to_handle(cmd_buffer->device),
			       &(VkFramebufferCreateInfo) {
				       .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
					       .attachmentCount = 1,
					       .pAttachments = (VkImageView[]) {
					       radv_image_view_to_handle(&tmp->iview),
				       },
					       .width = width,
							.height = height,
							.layers = 1
							}, &cmd_buffer->pool->alloc, &tmp->fb);
}

static void
blit2d_unbind_dst(struct radv_cmd_buffer *cmd_buffer,
                  struct blit2d_dst_temps *tmp)
{
	VkDevice vk_device = radv_device_to_handle(cmd_buffer->device);
	radv_DestroyFramebuffer(vk_device, tmp->fb, &cmd_buffer->pool->alloc);
	radv_DestroyImage(vk_device, tmp->image, &cmd_buffer->pool->alloc);
}

void
radv_meta_end_blit2d(struct radv_cmd_buffer *cmd_buffer,
		     struct radv_meta_saved_state *save)
{
	radv_meta_restore(save, cmd_buffer);
}

void
radv_meta_begin_blit2d(struct radv_cmd_buffer *cmd_buffer,
		       struct radv_meta_saved_state *save)
{
	radv_meta_save(save, cmd_buffer, 0);
}

static void
bind_pipeline(struct radv_cmd_buffer *cmd_buffer,
              enum blit2d_dst_type dst_type)
{
	VkPipeline pipeline =
		cmd_buffer->device->meta_state.blit2d.pipelines[dst_type];

	if (cmd_buffer->state.pipeline != radv_pipeline_from_handle(pipeline)) {
		radv_CmdBindPipeline(radv_cmd_buffer_to_handle(cmd_buffer),
				     VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	}
}

static void
radv_meta_blit2d_normal_dst(struct radv_cmd_buffer *cmd_buffer,
			    struct radv_meta_blit2d_surf *src,
			    struct radv_meta_blit2d_surf *dst,
			    unsigned num_rects,
			    struct radv_meta_blit2d_rect *rects)
{
	struct radv_device *device = cmd_buffer->device;

	for (unsigned r = 0; r < num_rects; ++r) {
		struct blit2d_src_temps src_temps;
		blit2d_bind_src(cmd_buffer, src, &rects[r], &src_temps);

		uint32_t offset = 0;
		struct blit2d_dst_temps dst_temps;
		blit2d_bind_dst(cmd_buffer, dst, offset, rects[r].dst_x + rects[r].width,
				rects[r].dst_y + rects[r].height, &dst_temps);

		struct blit_vb_data {
			float pos[2];
			float tex_coord[2];
		} vb_data[3];

		unsigned vb_size = 3 * sizeof(*vb_data);

		vb_data[0] = (struct blit_vb_data) {
			.pos = {
				rects[r].dst_x,
				rects[r].dst_y,
			},
			.tex_coord = {
				rects[r].src_x,
				rects[r].src_y,
			},
		};

		vb_data[1] = (struct blit_vb_data) {
			.pos = {
				rects[r].dst_x,
				rects[r].dst_y + rects[r].height,
			},
			.tex_coord = {
				rects[r].src_x,
				rects[r].src_y + rects[r].height,
			},
		};

		vb_data[2] = (struct blit_vb_data) {
			.pos = {
				rects[r].dst_x + rects[r].width,
				rects[r].dst_y,
			},
			.tex_coord = {
				rects[r].src_x + rects[r].width,
				rects[r].src_y,
			},
		};

		radv_cmd_buffer_upload_data(cmd_buffer, vb_size, 16, vb_data, &offset);

		struct radv_buffer vertex_buffer = {
			.device = device,
			.size = vb_size,
			.bo = &cmd_buffer->upload.upload_bo,
			.offset = offset,
		};

		radv_CmdBindVertexBuffers(radv_cmd_buffer_to_handle(cmd_buffer), 0, 1,
					  (VkBuffer[]) {
						  radv_buffer_to_handle(&vertex_buffer),
							  },
					  (VkDeviceSize[]) {
						  0,
							  });

		RADV_CALL(CmdBeginRenderPass)(radv_cmd_buffer_to_handle(cmd_buffer),
					      &(VkRenderPassBeginInfo) {
						      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
							      .renderPass = device->meta_state.blit2d.render_pass,
							      .framebuffer = dst_temps.fb,
							      .renderArea = {
							      .offset = { rects[r].dst_x, rects[r].dst_y, },
							      .extent = { rects[r].width, rects[r].height },
						      },
							      .clearValueCount = 0,
								       .pClearValues = NULL,
								       }, VK_SUBPASS_CONTENTS_INLINE);

		bind_pipeline(cmd_buffer, BLIT2D_DST_TYPE_NORMAL);

		RADV_CALL(CmdDraw)(radv_cmd_buffer_to_handle(cmd_buffer), 3, 1, 0, 0);

		RADV_CALL(CmdEndRenderPass)(radv_cmd_buffer_to_handle(cmd_buffer));

		/* At the point where we emit the draw call, all data from the
		 * descriptor sets, etc. has been used.  We are free to delete it.
		 */
		blit2d_unbind_src(cmd_buffer, &src_temps);
		blit2d_unbind_dst(cmd_buffer, &dst_temps);
	}
}

void
radv_meta_blit2d(struct radv_cmd_buffer *cmd_buffer,
		 struct radv_meta_blit2d_surf *src,
		 struct radv_meta_blit2d_surf *dst,
		 unsigned num_rects,
		 struct radv_meta_blit2d_rect *rects)
{
	if (dst->bs % 3 == 0) {
		radv_finishme("Blitting to RGB destinations not yet supported");
		return;
	} else {
		assert(util_is_power_of_two(dst->bs));
		radv_meta_blit2d_normal_dst(cmd_buffer, src, dst,
					    num_rects, rects);
	}
}

static nir_shader *
build_nir_vertex_shader(void)
{
	const struct glsl_type *vec4 = glsl_vec4_type();
	const struct glsl_type *vec2 = glsl_vector_type(GLSL_TYPE_FLOAT, 2);
	nir_builder b;

	nir_builder_init_simple_shader(&b, NULL, MESA_SHADER_VERTEX, NULL);
	b.shader->info.name = ralloc_strdup(b.shader, "meta_blit_vs");

	nir_variable *pos_in = nir_variable_create(b.shader, nir_var_shader_in,
						   vec4, "a_pos");
	pos_in->data.location = VERT_ATTRIB_GENERIC0;
	nir_variable *pos_out = nir_variable_create(b.shader, nir_var_shader_out,
						    vec4, "gl_Position");
	pos_out->data.location = VARYING_SLOT_POS;
	nir_copy_var(&b, pos_out, pos_in);

	nir_variable *tex_pos_in = nir_variable_create(b.shader, nir_var_shader_in,
						       vec2, "a_tex_pos");
	tex_pos_in->data.location = VERT_ATTRIB_GENERIC1;
	nir_variable *tex_pos_out = nir_variable_create(b.shader, nir_var_shader_out,
							vec2, "v_tex_pos");
	tex_pos_out->data.location = VARYING_SLOT_VAR0;
	tex_pos_out->data.interpolation = INTERP_MODE_SMOOTH;
	nir_copy_var(&b, tex_pos_out, tex_pos_in);

	return b.shader;
}

typedef nir_ssa_def* (*texel_fetch_build_func)(struct nir_builder *,
                                               struct radv_device *,
                                               nir_ssa_def *);

static nir_ssa_def *
build_nir_texel_fetch(struct nir_builder *b, struct radv_device *device,
                      nir_ssa_def *tex_pos)
{
	const struct glsl_type *sampler_type =
		glsl_sampler_type(GLSL_SAMPLER_DIM_2D, false, false, GLSL_TYPE_FLOAT);
	nir_variable *sampler = nir_variable_create(b->shader, nir_var_uniform,
						    sampler_type, "s_tex");
	sampler->data.descriptor_set = 0;
	sampler->data.binding = 0;

	nir_tex_instr *tex = nir_tex_instr_create(b->shader, 2);
	tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
	tex->op = nir_texop_txf;
	tex->src[0].src_type = nir_tex_src_coord;
	tex->src[0].src = nir_src_for_ssa(tex_pos);
	tex->src[1].src_type = nir_tex_src_lod;
	tex->src[1].src = nir_src_for_ssa(nir_imm_int(b, 0));
	tex->dest_type = nir_type_float; /* TODO */
	tex->is_array = false;
	tex->coord_components = 2;
	tex->texture = nir_deref_var_create(tex, sampler);
	tex->sampler = NULL;

	nir_ssa_dest_init(&tex->instr, &tex->dest, 4, 32, "tex");
	nir_builder_instr_insert(b, &tex->instr);

	return &tex->dest.ssa;
}

static const VkPipelineVertexInputStateCreateInfo normal_vi_create_info = {
	.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
	.vertexBindingDescriptionCount = 1,
	.pVertexBindingDescriptions = (VkVertexInputBindingDescription[]) {
		{
			.binding = 0,
			.stride = 4 * sizeof(float),
			.inputRate = VK_VERTEX_INPUT_RATE_VERTEX
		},
	},
	.vertexAttributeDescriptionCount = 2,
	.pVertexAttributeDescriptions = (VkVertexInputAttributeDescription[]) {
		{
			/* Position */
			.location = 0,
			.binding = 0,
			.format = VK_FORMAT_R32G32_SFLOAT,
			.offset = 0
		},
		{
			/* Texture Coordinate */
			.location = 1,
			.binding = 0,
			.format = VK_FORMAT_R32G32_SFLOAT,
			.offset = 8
		},
	},
};

static nir_shader *
build_nir_copy_fragment_shader(struct radv_device *device,
                               texel_fetch_build_func txf_func)
{
	const struct glsl_type *vec4 = glsl_vec4_type();
	const struct glsl_type *vec2 = glsl_vector_type(GLSL_TYPE_FLOAT, 2);
	nir_builder b;

	nir_builder_init_simple_shader(&b, NULL, MESA_SHADER_FRAGMENT, NULL);
	b.shader->info.name = ralloc_strdup(b.shader, "meta_blit2d_fs");

	nir_variable *tex_pos_in = nir_variable_create(b.shader, nir_var_shader_in,
						       vec2, "v_tex_pos");
	tex_pos_in->data.location = VARYING_SLOT_VAR0;

	nir_variable *color_out = nir_variable_create(b.shader, nir_var_shader_out,
						      vec4, "f_color");
	color_out->data.location = FRAG_RESULT_DATA0;

	nir_ssa_def *pos_int = nir_f2i(&b, nir_load_var(&b, tex_pos_in));
	unsigned swiz[4] = { 0, 1 };
	nir_ssa_def *tex_pos = nir_swizzle(&b, pos_int, swiz, 2, false);

	nir_ssa_def *color = txf_func(&b, device, tex_pos);
	nir_store_var(&b, color_out, color, 0xf);

	return b.shader;
}

void
radv_device_finish_meta_blit2d_state(struct radv_device *device)
{
	if (device->meta_state.blit2d.render_pass) {
		radv_DestroyRenderPass(radv_device_to_handle(device),
				       device->meta_state.blit2d.render_pass,
				       &device->meta_state.alloc);
	}

	if (device->meta_state.blit2d.img_p_layout) {
		radv_DestroyPipelineLayout(radv_device_to_handle(device),
					   device->meta_state.blit2d.img_p_layout,
					   &device->meta_state.alloc);
	}

	if (device->meta_state.blit2d.img_ds_layout) {
		radv_DestroyDescriptorSetLayout(radv_device_to_handle(device),
						device->meta_state.blit2d.img_ds_layout,
						&device->meta_state.alloc);
	}

	for (unsigned dst = 0; dst < BLIT2D_NUM_DST_TYPES; dst++) {
		if (device->meta_state.blit2d.pipelines[dst]) {
			radv_DestroyPipeline(radv_device_to_handle(device),
					     device->meta_state.blit2d.pipelines[dst],
					     &device->meta_state.alloc);
		}
	}
}

static VkResult
blit2d_init_pipeline(struct radv_device *device,
                     enum blit2d_dst_type dst_type)
{
	VkResult result;

	texel_fetch_build_func src_func = build_nir_texel_fetch;

	const VkPipelineVertexInputStateCreateInfo *vi_create_info;
	struct radv_shader_module fs = { .nir = NULL };
	switch (dst_type) {
	case BLIT2D_DST_TYPE_NORMAL:
		fs.nir = build_nir_copy_fragment_shader(device, src_func);
		vi_create_info = &normal_vi_create_info;
		break;
	case BLIT2D_DST_TYPE_RGB:
		/* Not yet supported */
	default:
		return VK_SUCCESS;
	}

	struct radv_shader_module vs = {
		.nir = build_nir_vertex_shader(),
	};

	VkPipelineShaderStageCreateInfo pipeline_shader_stages[] = {
		{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_VERTEX_BIT,
			.module = radv_shader_module_to_handle(&vs),
			.pName = "main",
			.pSpecializationInfo = NULL
		}, {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
			.module = radv_shader_module_to_handle(&fs),
			.pName = "main",
			.pSpecializationInfo = NULL
		},
	};

	const VkGraphicsPipelineCreateInfo vk_pipeline_info = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.stageCount = ARRAY_SIZE(pipeline_shader_stages),
		.pStages = pipeline_shader_stages,
		.pVertexInputState = vi_create_info,
		.pInputAssemblyState = &(VkPipelineInputAssemblyStateCreateInfo) {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
			.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
			.primitiveRestartEnable = false,
		},
		.pViewportState = &(VkPipelineViewportStateCreateInfo) {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
			.viewportCount = 1,
			.scissorCount = 1,
		},
		.pRasterizationState = &(VkPipelineRasterizationStateCreateInfo) {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
			.rasterizerDiscardEnable = false,
			.polygonMode = VK_POLYGON_MODE_FILL,
			.cullMode = VK_CULL_MODE_NONE,
			.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE
		},
		.pMultisampleState = &(VkPipelineMultisampleStateCreateInfo) {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
			.rasterizationSamples = 1,
			.sampleShadingEnable = false,
			.pSampleMask = (VkSampleMask[]) { UINT32_MAX },
		},
		.pColorBlendState = &(VkPipelineColorBlendStateCreateInfo) {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
			.attachmentCount = 1,
			.pAttachments = (VkPipelineColorBlendAttachmentState []) {
				{ .colorWriteMask =
				  VK_COLOR_COMPONENT_A_BIT |
				  VK_COLOR_COMPONENT_R_BIT |
				  VK_COLOR_COMPONENT_G_BIT |
				  VK_COLOR_COMPONENT_B_BIT },
			}
		},
		.pDynamicState = &(VkPipelineDynamicStateCreateInfo) {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
			.dynamicStateCount = 9,
			.pDynamicStates = (VkDynamicState[]) {
				VK_DYNAMIC_STATE_VIEWPORT,
				VK_DYNAMIC_STATE_SCISSOR,
				VK_DYNAMIC_STATE_LINE_WIDTH,
				VK_DYNAMIC_STATE_DEPTH_BIAS,
				VK_DYNAMIC_STATE_BLEND_CONSTANTS,
				VK_DYNAMIC_STATE_DEPTH_BOUNDS,
				VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
				VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
				VK_DYNAMIC_STATE_STENCIL_REFERENCE,
			},
		},
		.flags = 0,
		.layout = device->meta_state.blit2d.img_p_layout,
		.renderPass = device->meta_state.blit2d.render_pass,
		.subpass = 0,
	};

	const struct radv_graphics_pipeline_create_info radv_pipeline_info = {
		.use_rectlist = true
	};

	result = radv_graphics_pipeline_create(radv_device_to_handle(device),
					       VK_NULL_HANDLE,
					       &vk_pipeline_info, &radv_pipeline_info,
					       &device->meta_state.alloc,
					       &device->meta_state.blit2d.pipelines[dst_type]);

	ralloc_free(vs.nir);
	ralloc_free(fs.nir);

	return result;
}

VkResult
radv_device_init_meta_blit2d_state(struct radv_device *device)
{
	VkResult result;

	zero(device->meta_state.blit2d);

	result = radv_CreateRenderPass(radv_device_to_handle(device),
				       &(VkRenderPassCreateInfo) {
					       .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
						       .attachmentCount = 1,
						       .pAttachments = &(VkAttachmentDescription) {
						       .format = VK_FORMAT_UNDEFINED, /* Our shaders don't care */
						       .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
						       .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
						       .initialLayout = VK_IMAGE_LAYOUT_GENERAL,
						       .finalLayout = VK_IMAGE_LAYOUT_GENERAL,
					       },
						       .subpassCount = 1,
								.pSubpasses = &(VkSubpassDescription) {
						       .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
						       .inputAttachmentCount = 0,
						       .colorAttachmentCount = 1,
						       .pColorAttachments = &(VkAttachmentReference) {
							       .attachment = 0,
							       .layout = VK_IMAGE_LAYOUT_GENERAL,
						       },
						       .pResolveAttachments = NULL,
						       .pDepthStencilAttachment = &(VkAttachmentReference) {
							       .attachment = VK_ATTACHMENT_UNUSED,
							       .layout = VK_IMAGE_LAYOUT_GENERAL,
						       },
						       .preserveAttachmentCount = 1,
						       .pPreserveAttachments = (uint32_t[]) { 0 },
					       },
								.dependencyCount = 0,
									 }, &device->meta_state.alloc, &device->meta_state.blit2d.render_pass);
	if (result != VK_SUCCESS)
		goto fail;

	result = radv_CreateDescriptorSetLayout(radv_device_to_handle(device),
						&(VkDescriptorSetLayoutCreateInfo) {
							.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
								.bindingCount = 1,
								.pBindings = (VkDescriptorSetLayoutBinding[]) {
								{
									.binding = 0,
									.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
									.descriptorCount = 1,
									.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
									.pImmutableSamplers = NULL
								},
							}
						}, &device->meta_state.alloc, &device->meta_state.blit2d.img_ds_layout);
	if (result != VK_SUCCESS)
		goto fail;

	result = radv_CreatePipelineLayout(radv_device_to_handle(device),
					   &(VkPipelineLayoutCreateInfo) {
						   .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
							   .setLayoutCount = 1,
							   .pSetLayouts = &device->meta_state.blit2d.img_ds_layout,
							   },
					   &device->meta_state.alloc, &device->meta_state.blit2d.img_p_layout);
	if (result != VK_SUCCESS)
		goto fail;

	for (unsigned dst = 0; dst < 1; /*BLIT2D_NUM_DST_TYPES;*/ dst++) {
		result = blit2d_init_pipeline(device, dst);
		if (result != VK_SUCCESS)
			goto fail;
	}

	return VK_SUCCESS;

fail:
	radv_device_finish_meta_blit2d_state(device);
	return result;
}
