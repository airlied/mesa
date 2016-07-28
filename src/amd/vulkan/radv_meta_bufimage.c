#include "radv_meta.h"
#include "nir/nir_builder.h"

static nir_shader *
build_nir_itob_compute_shader(struct radv_device *dev)
{
	nir_builder b;
	const struct glsl_type *vec4 = glsl_vec4_type();
	const struct glsl_type *sampler_type = glsl_sampler_type(GLSL_SAMPLER_DIM_2D,
								 false,
								 false,
								 GLSL_TYPE_FLOAT);
	const struct glsl_type *img_type = glsl_sampler_type(GLSL_SAMPLER_DIM_BUF,
							     false,
							     false,
							     GLSL_TYPE_FLOAT);
	nir_builder_init_simple_shader(&b, NULL, MESA_SHADER_COMPUTE, NULL);
	b.shader->info.name = ralloc_strdup(b.shader, "meta_itob_cs");
	b.shader->info.cs.local_size[0] = 4;
	b.shader->info.cs.local_size[1] = 1;
	b.shader->info.cs.local_size[2] = 1;
	nir_variable *input_img = nir_variable_create(b.shader, nir_var_uniform,
						      sampler_type, "s_tex");
	input_img->data.descriptor_set = 0;
	input_img->data.binding = 0;

	nir_variable *output_img = nir_variable_create(b.shader, nir_var_uniform,
						       img_type, "out_img");
	output_img->data.descriptor_set = 0;
	output_img->data.binding = 1;

	nir_ssa_def *invoc_id = nir_load_system_value(&b, nir_intrinsic_load_local_invocation_id, 0);
	nir_ssa_def *wg_id = nir_load_system_value(&b, nir_intrinsic_load_work_group_id, 0);
	nir_ssa_def *wg_size = nir_load_system_value(&b, nir_intrinsic_load_num_work_groups, 0);
	nir_ssa_def *block_size = nir_imm_ivec4(&b,
						b.shader->info.cs.local_size[0],
						b.shader->info.cs.local_size[1],
						b.shader->info.cs.local_size[2], 0);

	nir_ssa_def *global_id = nir_iadd(&b, nir_imul(&b, wg_id, block_size), invoc_id);
	nir_tex_instr *tex = nir_tex_instr_create(b.shader, 2);
	tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
	tex->op = nir_texop_txf;
	tex->src[0].src_type = nir_tex_src_coord;
	tex->src[0].src = nir_src_for_ssa(global_id);
	tex->src[1].src_type = nir_tex_src_lod;
	tex->src[1].src = nir_src_for_ssa(nir_imm_int(&b, 0));
	tex->dest_type = nir_type_float;
	tex->is_array = false;
	tex->coord_components = 2;
	tex->texture = nir_deref_var_create(tex, input_img);
	tex->sampler = NULL;

	nir_ssa_dest_init(&tex->instr, &tex->dest, 4, 32, "tex");
	nir_builder_instr_insert(&b, &tex->instr);

	nir_ssa_def *pos_x = nir_channel(&b, global_id, 0);
	nir_ssa_def *pos_y = nir_channel(&b, global_id, 1);
	nir_ssa_def *width = nir_channel(&b, nir_imul(&b, wg_size, block_size), 0);

	nir_ssa_def *tmp = nir_imul(&b, pos_y, width);
	tmp = nir_iadd(&b, tmp, pos_x);

	nir_ssa_def *coord = nir_vec4(&b, tmp, tmp, tmp, tmp);

	nir_ssa_def *outval = &tex->dest.ssa;
	nir_intrinsic_instr *store = nir_intrinsic_instr_create(b.shader, nir_intrinsic_image_store);
	store->src[0] = nir_src_for_ssa(coord);
	store->src[1] = nir_src_for_ssa(nir_ssa_undef(&b, 1, 32));
	store->src[2] = nir_src_for_ssa(outval);
	store->variables[0] = nir_deref_var_create(store, output_img);

	nir_builder_instr_insert(&b, &store->instr);
	return b.shader;
}

/* Image to buffer - don't write use image accessors */
static VkResult
radv_device_init_meta_itob_state(struct radv_device *device)
{
	VkResult result;
	struct radv_shader_module cs = { .nir = NULL };

	zero(device->meta_state.itob);

	cs.nir = build_nir_itob_compute_shader(device);

	/*
	 * two descriptors one for the image being sampled
	 * one for the buffer being written.
	 */
	VkDescriptorSetLayoutCreateInfo ds_create_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 2,
		.pBindings = (VkDescriptorSetLayoutBinding[]) {
			{
				.binding = 0,
				.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
				.descriptorCount = 1,
				.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
				.pImmutableSamplers = NULL
			},
			{
				.binding = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,
				.descriptorCount = 1,
				.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
				.pImmutableSamplers = NULL
			},
		}
	};

	result = radv_CreateDescriptorSetLayout(radv_device_to_handle(device),
						&ds_create_info,
						&device->meta_state.alloc,
						&device->meta_state.itob.img_ds_layout);
	if (result != VK_SUCCESS)
		goto fail;


	VkPipelineLayoutCreateInfo pl_create_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &device->meta_state.itob.img_ds_layout,
	};

	result = radv_CreatePipelineLayout(radv_device_to_handle(device),
					  &pl_create_info,
					  &device->meta_state.alloc,
					  &device->meta_state.itob.img_p_layout);
	if (result != VK_SUCCESS)
		goto fail;

	/* compute shader */

	VkPipelineShaderStageCreateInfo pipeline_shader_stage = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		.stage = VK_SHADER_STAGE_COMPUTE_BIT,
		.module = radv_shader_module_to_handle(&cs),
		.pName = "main",
		.pSpecializationInfo = NULL,
	};

	VkComputePipelineCreateInfo vk_pipeline_info = {
		.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = pipeline_shader_stage,
		.flags = 0,
		.layout = device->meta_state.itob.img_p_layout,
	};

	result = radv_CreateComputePipelines(radv_device_to_handle(device),
					     NULL, 1, &vk_pipeline_info,
					     NULL, &device->meta_state.itob.pipeline);
	if (result != VK_SUCCESS)
		goto fail;
	return VK_SUCCESS;
fail:
	return result;
}

static void
radv_device_finish_meta_itob_state(struct radv_device *device)
{
	if (device->meta_state.itob.img_p_layout) {
		radv_DestroyPipelineLayout(radv_device_to_handle(device),
					   device->meta_state.itob.img_p_layout,
					   &device->meta_state.alloc);
	}
	if (device->meta_state.itob.img_ds_layout) {
		radv_DestroyDescriptorSetLayout(radv_device_to_handle(device),
						device->meta_state.itob.img_ds_layout,
						&device->meta_state.alloc);
	}
	if (device->meta_state.itob.pipeline) {
		radv_DestroyPipeline(radv_device_to_handle(device),
				     device->meta_state.itob.pipeline,
				     &device->meta_state.alloc);
	}
}

void
radv_device_finish_meta_bufimage_state(struct radv_device *device)
{
	radv_device_finish_meta_itob_state(device);
}

VkResult
radv_device_init_meta_bufimage_state(struct radv_device *device)
{
	VkResult result;

	result = radv_device_init_meta_itob_state(device);
	if (result != VK_SUCCESS)
		return result;
	return VK_SUCCESS;
}

void
radv_meta_begin_bufimage(struct radv_cmd_buffer *cmd_buffer,
			 struct radv_meta_saved_state *save)
{
	radv_meta_save(save, cmd_buffer, 0);
}

void
radv_meta_end_bufimage(struct radv_cmd_buffer *cmd_buffer,
		       struct radv_meta_saved_state *save)
{
	radv_meta_restore(save, cmd_buffer);
}
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

static void
create_bview(struct radv_cmd_buffer *cmd_buffer,
	     struct radv_buffer *buffer,
	     unsigned bs,
	     struct radv_buffer_view *bview)
{
	radv_buffer_view_init(bview, cmd_buffer->device,
			      &(VkBufferViewCreateInfo) {
				      .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
				      .flags = 0,
				      .buffer = radv_buffer_to_handle(buffer),
				      .format = vk_format_for_size(bs),
				      .offset = 0,
					      .range = VK_WHOLE_SIZE,
					      }, cmd_buffer);

}

struct itob_temps {
	VkImage src_image;
	struct radv_image_view src_iview;

	struct radv_buffer_view dst_bview;
	VkDescriptorSet set;
};

static void
itob_bind_src_image(struct radv_cmd_buffer *cmd_buffer,
		   struct radv_meta_blit2d_surf *src,
		   struct radv_meta_blit2d_rect *rect,
		   struct itob_temps *tmp)
{
	struct radv_device *device = cmd_buffer->device;
	uint32_t offset = 0;

	create_iview(cmd_buffer, src, offset, VK_IMAGE_USAGE_SAMPLED_BIT,
		     rect->src_x + rect->width, rect->src_y + rect->height,
		     &tmp->src_image, &tmp->src_iview);

}

static void
itob_bind_dst_buffer(struct radv_cmd_buffer *cmd_buffer,
		     struct radv_buffer *buffer,
		     struct radv_meta_blit2d_rect *rect,
		     struct itob_temps *tmp)
{
	create_bview(cmd_buffer, buffer, 4, &tmp->dst_bview);
}

static void
itob_bind_descriptors(struct radv_cmd_buffer *cmd_buffer,
		      struct itob_temps *tmp)
{
	struct radv_device *device = cmd_buffer->device;
	VkDevice vk_device = radv_device_to_handle(cmd_buffer->device);

	radv_temp_descriptor_set_create(device, cmd_buffer,
					device->meta_state.itob.img_ds_layout,
					&tmp->set);

	radv_UpdateDescriptorSets(vk_device,
				  2, /* writeCount */
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
								  .imageView = radv_image_view_to_handle(&tmp->src_iview),
								  .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
							  },
						  }
					  },
					  {
						  .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
						  .dstSet = tmp->set,
						  .dstBinding = 1,
						  .dstArrayElement = 0,
						  .descriptorCount = 1,
						  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,
						  .pTexelBufferView = (VkBufferView[])  { radv_buffer_view_to_handle(&tmp->dst_bview) },
					  }
				  }, 0, NULL);

	radv_CmdBindDescriptorSets(radv_cmd_buffer_to_handle(cmd_buffer),
				   VK_PIPELINE_BIND_POINT_COMPUTE,
				   device->meta_state.itob.img_p_layout, 0, 1,
				   &tmp->set, 0, NULL);
}

static void
itob_unbind_src_image(struct radv_cmd_buffer *cmd_buffer,
		      struct itob_temps *temps)
{
	radv_DestroyImage(radv_device_to_handle(cmd_buffer->device),
			  temps->src_image, &cmd_buffer->pool->alloc);
}

static void
bind_pipeline(struct radv_cmd_buffer *cmd_buffer)
{
	VkPipeline pipeline =
		cmd_buffer->device->meta_state.itob.pipeline;

	if (cmd_buffer->state.compute_pipeline != radv_pipeline_from_handle(pipeline)) {
		radv_CmdBindPipeline(radv_cmd_buffer_to_handle(cmd_buffer),
				     VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
	}
}

void
radv_meta_image_to_buffer(struct radv_cmd_buffer *cmd_buffer,
			  struct radv_meta_blit2d_surf *src,
			  struct radv_buffer *dst,
			  unsigned num_rects,
			  struct radv_meta_blit2d_rect *rects)
{
	struct radv_device *device = cmd_buffer->device;

	for (unsigned r = 0; r < num_rects; ++r) {
		struct itob_temps temps;

		itob_bind_src_image(cmd_buffer, src, &rects[r], &temps);
		itob_bind_dst_buffer(cmd_buffer, dst, &rects[r], &temps);
		itob_bind_descriptors(cmd_buffer, &temps);

		bind_pipeline(cmd_buffer);

		radv_CmdDispatch(radv_cmd_buffer_to_handle(cmd_buffer), rects[r].width / 4, rects[r].height, 1);
		radv_temp_descriptor_set_destroy(cmd_buffer->device, temps.set);
		itob_unbind_src_image(cmd_buffer, &temps);
	}

}
