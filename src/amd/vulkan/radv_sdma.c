/*
 * Copyright © 2016 Red Hat.
 *
 * based on cik_sdma.c:
 * Copyright 2014,2015 Advanced Micro Devices, Inc.
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
#include "sid.h"
#include "radv_cs.h"

#define CIK_MAX_DIM (1<<14)
static unsigned minify_as_blocks(unsigned width, unsigned level, unsigned blk_w)
{
	width = radv_minify(width, level);
	return DIV_ROUND_UP(width, blk_w);
}

static const struct legacy_surf_level *get_base_level_info(const struct radv_image *img,
							   bool is_stencil, int base_mip_level)
{
	if (is_stencil)
		return &img->planes[0].surface.u.legacy.stencil_level[base_mip_level];
	return &img->planes[0].surface.u.legacy.level[base_mip_level];
}

static unsigned encode_tile_info_gfx6(struct radeon_info *info,
				      struct radv_image *image, unsigned level,
				      bool set_bpp)
{
	unsigned tile_index = image->planes[0].surface.u.legacy.tiling_index[level];
	unsigned macro_tile_index = image->planes[0].surface.u.legacy.macro_tile_index;
	unsigned tile_mode = info->si_tile_mode_array[tile_index];
	unsigned macro_tile_mode = info->cik_macrotile_mode_array[macro_tile_index];

	return (set_bpp ? util_logbase2(image->planes[0].surface.bpe) : 0) |
		(G_009910_ARRAY_MODE(tile_mode) << 3) |
		(G_009910_MICRO_TILE_MODE_NEW(tile_mode) << 8) |
		/* Non-depth modes don't have TILE_SPLIT set. */
		((util_logbase2(image->planes[0].surface.u.legacy.tile_split >> 6)) << 11) |
		(G_009990_BANK_WIDTH(macro_tile_mode) << 15) |
		(G_009990_BANK_HEIGHT(macro_tile_mode) << 18) |
		(G_009990_NUM_BANKS(macro_tile_mode) << 21) |
		(G_009990_MACRO_TILE_ASPECT(macro_tile_mode) << 24) |
		(G_009910_PIPE_CONFIG(tile_mode) << 26);
}

static unsigned encode_tile_info_gfx9(struct radeon_info *info,
				      struct radv_image *image, unsigned level,
				      bool set_bpp)
{
	return (util_logbase2(image->planes[0].surface.bpe)) |
		(image->planes[0].surface.u.gfx9.surf.swizzle_mode << 3) |
		((image->type == VK_IMAGE_TYPE_3D ? 2 : 1) << 9) |
		(image->planes[0].surface.u.gfx9.surf.epitch << 16);
}

static unsigned encode_tile_info(struct radv_cmd_buffer *cmd_buffer,
				 struct radv_image *image, unsigned level,
				 bool set_bpp)
{
	struct radeon_info *info = &cmd_buffer->device->physical_device->rad_info;
	if (info->chip_class >= GFX9)
		return encode_tile_info_gfx9(info, image, level, set_bpp);
	else
		return encode_tile_info_gfx6(info, image, level, set_bpp);
}

/* The hw can read outside of the given linear buffer bounds,
 * or access those pages but not touch the memory in case
 * of writes. (it still causes a VM fault)
 *
 * Out-of-bounds memory access or page directory access must
 * be prevented.
 */
static bool
linear_buffer_workaround(struct radv_cmd_buffer *cmd_buffer,
			 struct radv_image *til_image,
			 const struct radv_transfer_per_image_info *til_info,
			 struct radv_image *lin_image,
			 const struct radv_transfer_per_image_info *lin_info,
			 uint32_t copy_width, uint32_t copy_height, uint32_t copy_depth,
			 uint32_t bpp)
{
	struct radeon_info *info = &cmd_buffer->device->physical_device->rad_info;
	unsigned til_tile_index = til_image->planes[0].surface.u.legacy.tiling_index[til_info->mip_level];
	unsigned til_tile_mode = info->si_tile_mode_array[til_tile_index];
	unsigned til_micro_mode = G_009910_MICRO_TILE_MODE_NEW(til_tile_mode);
	unsigned granularity;
	int64_t start_linear_address, end_linear_address;

	/* Deduce the size of reads from the linear surface. */
	switch (til_micro_mode) {
	case V_009910_ADDR_SURF_DISPLAY_MICRO_TILING:
		granularity = bpp == 1 ? 64 / (8*bpp) :
		128 / (8*bpp);
		break;
	case V_009910_ADDR_SURF_THIN_MICRO_TILING:
	case V_009910_ADDR_SURF_DEPTH_MICRO_TILING:
		if (0 /* TODO: THICK microtiling */)
			granularity = bpp == 1 ? 32 / (8*bpp) :
				bpp == 2 ? 64 / (8*bpp) :
				bpp <= 8 ? 128 / (8*bpp) :
				256 / (8*bpp);
		else
			granularity = bpp <= 2 ? 64 / (8*bpp) :
				bpp <= 8 ? 128 / (8*bpp) :
				256 / (8*bpp);
				break;
	default:
		return false;
	}

		/* The linear reads start at tiled_x & ~(granularity - 1).
	 * If linear_x == 0 && tiled_x % granularity != 0, the hw
	 * starts reading from an address preceding linear_address!!!
	 */
	start_linear_address =
		lin_image->planes[0].surface.u.legacy.level[lin_info->mip_level].offset +
		bpp * (lin_info->offset.z * lin_info->slice_pitch +
		       lin_info->offset.y * lin_info->pitch +
		       lin_info->offset.x);
	start_linear_address -= (int)(bpp * (til_info->offset.x % granularity));

	end_linear_address =
		lin_image->planes[0].surface.u.legacy.level[lin_info->mip_level].offset +
		bpp * ((lin_info->offset.z + copy_depth - 1) * lin_info->slice_pitch +
		       (lin_info->offset.y + copy_height - 1) * lin_info->pitch +
		       (lin_info->offset.x + copy_width));

	if ((til_info->offset.x + copy_width) % granularity)
		end_linear_address += granularity -
			(til_info->offset.x + copy_width) % granularity;

	if (start_linear_address < 0 ||
	    end_linear_address > lin_image->planes[0].surface.surf_size) {
		return false;
	}
	return true;
}

static void
radv_sdma_copy_one_lin_to_lin(struct radv_cmd_buffer *cmd_buffer,
			      const struct radv_transfer_image_buffer_info *info,
			      bool buf2img)
{
	uint64_t src_va, dst_va;
	uint32_t src_xy, src_z_pitch, src_slice_pitch;
	uint32_t dst_xy, dst_z_pitch, dst_slice_pitch;

	src_va = buf2img ? info->buf_info.va : info->image_info.va;
	dst_va = buf2img ? info->image_info.va : info->buf_info.va;

	uint32_t img_xy = info->image_info.offset.x | (info->image_info.offset.y << 16);
	uint32_t img_z_pitch = info->image_info.offset.z | ((info->image_info.pitch - 1) << 16);
	uint32_t buf_z_pitch = ((info->buf_info.pitch - 1) << 16);
	src_xy = buf2img ? 0 : img_xy;
	dst_xy = buf2img ? img_xy : 0;

	src_z_pitch = buf2img ? buf_z_pitch : img_z_pitch;
	dst_z_pitch = buf2img ? img_z_pitch : buf_z_pitch;

	src_slice_pitch = buf2img ? (info->buf_info.slice_pitch - 1) : (info->image_info.slice_pitch - 1);
	dst_slice_pitch = buf2img ? (info->image_info.slice_pitch - 1) : (info->buf_info.slice_pitch - 1);

	radeon_check_space(cmd_buffer->device->ws, cmd_buffer->cs, 13);
	radeon_emit(cmd_buffer->cs, CIK_SDMA_PACKET(CIK_SDMA_OPCODE_COPY,
						    CIK_SDMA_COPY_SUB_OPCODE_LINEAR_SUB_WINDOW, 0) |
		    (util_logbase2(info->image_info.bpp) << 29));
	radeon_emit(cmd_buffer->cs, src_va);
	radeon_emit(cmd_buffer->cs, src_va >> 32);
	radeon_emit(cmd_buffer->cs, src_xy);
	radeon_emit(cmd_buffer->cs, src_z_pitch);
	radeon_emit(cmd_buffer->cs, src_slice_pitch);
	radeon_emit(cmd_buffer->cs, dst_va);
	radeon_emit(cmd_buffer->cs, dst_va >> 32);
	radeon_emit(cmd_buffer->cs, dst_xy);
	radeon_emit(cmd_buffer->cs, dst_z_pitch);
	radeon_emit(cmd_buffer->cs, dst_slice_pitch);
	radeon_emit(cmd_buffer->cs, (info->extent.width - 1) | ((info->extent.height - 1) << 16));
	radeon_emit(cmd_buffer->cs, (info->extent.depth - 1));
}

static void
radv_sdma_copy_one_lin_to_lin_cik(struct radv_cmd_buffer *cmd_buffer,
				  const struct radv_transfer_image_buffer_info *info,
				  bool buf2img)
{
	uint64_t src_va, dst_va;
	uint32_t src_xy, src_z_pitch, src_slice_pitch;
	uint32_t dst_xy, dst_z_pitch, dst_slice_pitch;
	int num_x_xfer = 1, num_y_xfer = 1;
	src_va = buf2img ? info->buf_info.va : info->image_info.va;
	dst_va = buf2img ? info->image_info.va : info->buf_info.va;

	uint32_t img_z_pitch = info->image_info.offset.z | ((info->image_info.pitch - 1) << 16);
	uint32_t buf_z_pitch = ((info->buf_info.pitch - 1) << 16);
	src_z_pitch = buf2img ? buf_z_pitch : img_z_pitch;
	dst_z_pitch = buf2img ? img_z_pitch : buf_z_pitch;

	src_slice_pitch = buf2img ? (info->buf_info.slice_pitch - 1) : (info->image_info.slice_pitch - 1);
	dst_slice_pitch = buf2img ? (info->image_info.slice_pitch - 1) : (info->buf_info.slice_pitch - 1);

	uint32_t width = info->extent.width;
	uint32_t height = info->extent.height;
	if (info->extent.width == CIK_MAX_DIM) {
		num_x_xfer++;
		width /= 2;
	}
	if (info->extent.height == CIK_MAX_DIM) {
		num_y_xfer++;
		height /= 2;
	}

	for (int x = 0; x < num_x_xfer; x++) {
		for (int y = 0; y < num_y_xfer; y++) {
			uint32_t img_xy = (info->image_info.offset.x + (x * width)) | ((info->image_info.offset.y + (y * height)) << 16);
			uint32_t buf_xy = (x * width) | ((y * height) << 16);
			src_xy = buf2img ? buf_xy : img_xy;
			dst_xy = buf2img ? img_xy : buf_xy;
			radeon_check_space(cmd_buffer->device->ws, cmd_buffer->cs, 13);
			radeon_emit(cmd_buffer->cs, CIK_SDMA_PACKET(CIK_SDMA_OPCODE_COPY,
								    CIK_SDMA_COPY_SUB_OPCODE_LINEAR_SUB_WINDOW, 0) |
				    (util_logbase2(info->image_info.bpp) << 29));
			radeon_emit(cmd_buffer->cs, src_va);
			radeon_emit(cmd_buffer->cs, src_va >> 32);
			radeon_emit(cmd_buffer->cs, src_xy);
			radeon_emit(cmd_buffer->cs, src_z_pitch);
			radeon_emit(cmd_buffer->cs, src_slice_pitch);
			radeon_emit(cmd_buffer->cs, dst_va);
			radeon_emit(cmd_buffer->cs, dst_va >> 32);
			radeon_emit(cmd_buffer->cs, dst_xy);
			radeon_emit(cmd_buffer->cs, dst_z_pitch);
			radeon_emit(cmd_buffer->cs, dst_slice_pitch);
			radeon_emit(cmd_buffer->cs, width | (height << 16));
			radeon_emit(cmd_buffer->cs, info->extent.depth);
		}
	}
}

static void
radv_sdma_copy_one_lin_to_tiled(struct radv_cmd_buffer *cmd_buffer,
				const struct radv_transfer_image_buffer_info *info,
				struct radv_image *image,
				bool buf2img)
{
	radeon_check_space(cmd_buffer->device->ws, cmd_buffer->cs, 14);

	unsigned copy_width = DIV_ROUND_UP(info->extent.width, image->planes[0].surface.blk_w);
	unsigned copy_height = DIV_ROUND_UP(info->extent.height, image->planes[0].surface.blk_h);
	unsigned copy_width_aligned = copy_width;

	unsigned dword0, dword4, dword5;

	dword0 = CIK_SDMA_PACKET(CIK_SDMA_OPCODE_COPY,
				 CIK_SDMA_COPY_SUB_OPCODE_TILED_SUB_WINDOW, 0) |
		(buf2img ? 0 : (1u << 31));

	dword4 = info->image_info.offset.z;
	if (cmd_buffer->device->physical_device->rad_info.chip_class >= GFX9) {
		dword4 |= (image->info.width - 1) << 16;
		dword5 = (image->info.height - 1) | ((image->info.depth - 1) << 16);
		dword0 |= (image->info.levels - 1) << 20;
		dword0 |= info->image_info.mip_level << 24;
	} else {
		unsigned pitch_tile_max = info->image_info.pitch / 8 - 1;
		unsigned slice_tile_max = info->image_info.slice_pitch / 64 - 1;

		dword4 |= (pitch_tile_max << 16);
		dword5 = slice_tile_max;
	}

	radeon_emit(cmd_buffer->cs, dword0);
	radeon_emit(cmd_buffer->cs, info->image_info.va);
	radeon_emit(cmd_buffer->cs, info->image_info.va >> 32);
	radeon_emit(cmd_buffer->cs, info->image_info.offset.x | (info->image_info.offset.y << 16));
	radeon_emit(cmd_buffer->cs, dword4);
	radeon_emit(cmd_buffer->cs, dword5);
	radeon_emit(cmd_buffer->cs, encode_tile_info(cmd_buffer, image, info->image_info.mip_level, true));
	radeon_emit(cmd_buffer->cs, info->buf_info.va);
	radeon_emit(cmd_buffer->cs, info->buf_info.va >> 32);
	radeon_emit(cmd_buffer->cs, 0/*x,y*/);
	radeon_emit(cmd_buffer->cs, ((info->buf_info.pitch - 1) << 16));
	radeon_emit(cmd_buffer->cs, info->buf_info.slice_pitch - 1);
	radeon_emit(cmd_buffer->cs, (copy_width_aligned - 1) | ((copy_height - 1) << 16));
	radeon_emit(cmd_buffer->cs, (info->extent.depth - 1));
}

static void
radv_sdma_copy_one_lin_to_tiled_cik(struct radv_cmd_buffer *cmd_buffer,
				    const struct radv_transfer_image_buffer_info *info,
				    struct radv_image *image,
				    bool buf2img)
{
	unsigned copy_width = DIV_ROUND_UP(info->extent.width, image->planes[0].surface.blk_w);
	unsigned copy_height = DIV_ROUND_UP(info->extent.height, image->planes[0].surface.blk_h);
	int num_x_xfers = 1, num_y_xfers = 1;
	unsigned dword0, dword4, dword5;

	dword0 = CIK_SDMA_PACKET(CIK_SDMA_OPCODE_COPY,
				 CIK_SDMA_COPY_SUB_OPCODE_TILED_SUB_WINDOW, 0) |
		(buf2img ? 0 : (1u << 31));

	dword4 = info->image_info.offset.z;
	unsigned pitch_tile_max = info->image_info.pitch / 8 - 1;
	unsigned slice_tile_max = info->image_info.slice_pitch / 64 - 1;

	dword4 |= (pitch_tile_max << 16);
	dword5 = slice_tile_max;

	if (copy_width == CIK_MAX_DIM) {
		num_x_xfers++;
		copy_width /= 2;
	}
	if ((info->image_info.offset.y + copy_height == CIK_MAX_DIM) && (copy_height > 1)) {
		num_y_xfers++;
		copy_height -= 1;
	}
	for (uint32_t x = 0; x < num_x_xfers; x++) {
		for (uint32_t y = 0; y < num_y_xfers; y++) {
			uint32_t img_xy, buf_xy;

			img_xy = info->image_info.offset.x + (x * copy_width);
			img_xy |= (info->image_info.offset.y + (y * copy_height)) << 16;
			buf_xy = (x * copy_width) | ((y * copy_height) << 16);
			radeon_check_space(cmd_buffer->device->ws, cmd_buffer->cs, 14);
			radeon_emit(cmd_buffer->cs, dword0);
			radeon_emit(cmd_buffer->cs, info->image_info.va);
			radeon_emit(cmd_buffer->cs, info->image_info.va >> 32);
			radeon_emit(cmd_buffer->cs, img_xy);
			radeon_emit(cmd_buffer->cs, dword4);
			radeon_emit(cmd_buffer->cs, dword5);
			radeon_emit(cmd_buffer->cs, encode_tile_info(cmd_buffer, image, info->image_info.mip_level, true));
			radeon_emit(cmd_buffer->cs, info->buf_info.va);
			radeon_emit(cmd_buffer->cs, info->buf_info.va >> 32);
			radeon_emit(cmd_buffer->cs, buf_xy);
			radeon_emit(cmd_buffer->cs, ((info->buf_info.pitch - 1) << 16));
			radeon_emit(cmd_buffer->cs, info->buf_info.slice_pitch - 1);
			radeon_emit(cmd_buffer->cs, copy_width | (((y == 0) ? copy_height : 1) << 16));
			radeon_emit(cmd_buffer->cs, info->extent.depth);
		}
	}
}

static void
radv_sdma_copy_image_lin_to_lin(struct radv_cmd_buffer *cmd_buffer,
				const struct radv_transfer_image_info *info,
				struct radv_image *src_image,
				struct radv_image *dst_image)
{
	radeon_check_space(cmd_buffer->device->ws, cmd_buffer->cs, 13);
	radeon_emit(cmd_buffer->cs, CIK_SDMA_PACKET(CIK_SDMA_OPCODE_COPY,
						    CIK_SDMA_COPY_SUB_OPCODE_LINEAR_SUB_WINDOW, 0) |
		    (util_logbase2(info->src_info.bpp) << 29));
	radeon_emit(cmd_buffer->cs, info->src_info.va);
	radeon_emit(cmd_buffer->cs, info->src_info.va >> 32);
	radeon_emit(cmd_buffer->cs, info->src_info.offset.x | (info->src_info.offset.y << 16));
	radeon_emit(cmd_buffer->cs, info->src_info.offset.z | ((info->src_info.pitch - 1) << 16));
	radeon_emit(cmd_buffer->cs, info->src_info.slice_pitch - 1);
	radeon_emit(cmd_buffer->cs, info->dst_info.va);
	radeon_emit(cmd_buffer->cs, info->dst_info.va >> 32);
	radeon_emit(cmd_buffer->cs, info->dst_info.offset.x | (info->dst_info.offset.y << 16));
	radeon_emit(cmd_buffer->cs, info->dst_info.offset.z | ((info->dst_info.pitch - 1) << 16));
	radeon_emit(cmd_buffer->cs, info->dst_info.slice_pitch - 1);
	radeon_emit(cmd_buffer->cs, (info->extent.width -1) | ((info->extent.height - 1) << 16));
	radeon_emit(cmd_buffer->cs, (info->extent.depth - 1));
}

static void
radv_sdma_copy_image_lin_to_lin_cik(struct radv_cmd_buffer *cmd_buffer,
				    const struct radv_transfer_image_info *info,
				    struct radv_image *src_image,
				    struct radv_image *dst_image)
{
	int num_x_xfer = 1, num_y_xfer = 1;

	uint32_t width = info->extent.width;
	uint32_t height = info->extent.height;
	if (width == CIK_MAX_DIM) {
		num_x_xfer++;
		width /= 2;
	}
	if (height == CIK_MAX_DIM) {
		num_y_xfer++;
		height /= 2;
	}
	for (uint32_t x = 0; x < num_x_xfer; x++) {
		for (uint32_t y = 0; y < num_y_xfer; y++) {
			uint32_t src_xy = (info->src_info.offset.x + (x * width)) | ((info->src_info.offset.y + (y * height)) << 16);
			uint32_t dst_xy = (info->dst_info.offset.x + (x * width)) | ((info->dst_info.offset.y + (y * height)) << 16);
			radeon_check_space(cmd_buffer->device->ws, cmd_buffer->cs, 13);
			radeon_emit(cmd_buffer->cs, CIK_SDMA_PACKET(CIK_SDMA_OPCODE_COPY,
								    CIK_SDMA_COPY_SUB_OPCODE_LINEAR_SUB_WINDOW, 0) |
				    (util_logbase2(info->src_info.bpp) << 29));
			radeon_emit(cmd_buffer->cs, info->src_info.va);
			radeon_emit(cmd_buffer->cs, info->src_info.va >> 32);
			radeon_emit(cmd_buffer->cs, src_xy);
			radeon_emit(cmd_buffer->cs, info->src_info.offset.z | ((info->src_info.pitch - 1) << 16));
			radeon_emit(cmd_buffer->cs, info->src_info.slice_pitch - 1);
			radeon_emit(cmd_buffer->cs, info->dst_info.va);
			radeon_emit(cmd_buffer->cs, info->dst_info.va >> 32);
			radeon_emit(cmd_buffer->cs, dst_xy);
			radeon_emit(cmd_buffer->cs, info->dst_info.offset.z | ((info->dst_info.pitch - 1) << 16));
			radeon_emit(cmd_buffer->cs, info->dst_info.slice_pitch - 1);
			radeon_emit(cmd_buffer->cs, width | (height << 16));
			radeon_emit(cmd_buffer->cs, info->extent.depth);
		}
	}
}

static void
radv_sdma_copy_image_lin_to_tiled(struct radv_cmd_buffer *cmd_buffer,
				  const struct radv_transfer_image_info *info,
				  struct radv_image *src_image,
				  struct radv_image *dst_image)
{
	const struct radv_transfer_per_image_info *lin_info, *til_info;
	struct radv_image *lin_image, *til_image;
	uint32_t dword0, dword4, dword5;
	unsigned lin_width, til_width;
	bool src_is_linear = src_image->planes[0].surface.is_linear;

	lin_info = src_is_linear ? &info->src_info : &info->dst_info;
	til_info = src_is_linear ? &info->dst_info : &info->src_info;

	lin_image = src_is_linear ? src_image : dst_image;
	til_image = src_is_linear ? dst_image : src_image;

	lin_width = minify_as_blocks(lin_image->info.width,
				     lin_info->mip_level, lin_image->planes[0].surface.blk_w);
	til_width = minify_as_blocks(til_image->info.width,
				     til_info->mip_level, til_image->planes[0].surface.blk_w);
	radeon_check_space(cmd_buffer->device->ws, cmd_buffer->cs, 14);

	assert(til_info->pitch % 8 == 0);
	assert(til_info->slice_pitch % 64 == 0);
	unsigned bpp = lin_info->bpp;
	unsigned xalign = MAX2(1, 4 / lin_info->bpp);
	unsigned copy_width = DIV_ROUND_UP(info->extent.width, til_image->planes[0].surface.blk_w);
	unsigned copy_height = DIV_ROUND_UP(info->extent.height, til_image->planes[0].surface.blk_h);
	unsigned copy_width_aligned = copy_width;
	unsigned copy_depth = info->extent.depth;

	/* If the region ends at the last pixel and is unaligned, we
	 * can copy the remainder of the line that is not visible to
	 * make it aligned.
	 */
	if (copy_width % xalign != 0 &&
	    lin_info->offset.x + copy_width == lin_width &&
	    lin_info->offset.x  + copy_width == til_width &&
	    lin_info->offset.x + align(copy_width, xalign) <= lin_info->pitch &&
	    til_info->offset.x  + align(copy_width, xalign) <= til_info->pitch)
		copy_width_aligned = align(copy_width, xalign);

	/* TODO HW Limitations - how do we handle those in vk? */

	if (cmd_buffer->device->physical_device->rad_info.chip_class < GFX9) {
		 bool ret = linear_buffer_workaround(cmd_buffer,
						     til_image, til_info,
						     lin_image, lin_info,
						     copy_width, copy_height, copy_depth,
						     bpp);

		 if (ret == false) {
			 cmd_buffer->record_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
			 return;
		 }
	}

	dword0 = CIK_SDMA_PACKET(CIK_SDMA_OPCODE_COPY,
				 CIK_SDMA_COPY_SUB_OPCODE_TILED_SUB_WINDOW, 0) |
		(src_is_linear ? 0 : (1u << 31));

	dword4 = til_info->offset.z;
	if (cmd_buffer->device->physical_device->rad_info.chip_class >= GFX9) {
		dword4 |= (til_image->info.width - 1) << 16;
		dword5 = (til_image->info.height - 1) | ((til_image->info.depth - 1) << 16);
		dword0 |= (til_image->info.levels - 1) << 20;
		dword0 |= til_info->mip_level << 24;
	} else {
		unsigned pitch_tile_max = til_info->pitch / 8 - 1;
		unsigned slice_tile_max = til_info->slice_pitch / 64 - 1;

		dword4 |= (pitch_tile_max << 16);
		dword5 = slice_tile_max;
	}
	radeon_emit(cmd_buffer->cs, dword0);
	radeon_emit(cmd_buffer->cs, til_info->va);
	radeon_emit(cmd_buffer->cs, til_info->va >> 32);
	radeon_emit(cmd_buffer->cs, til_info->offset.x | (til_info->offset.y << 16));
	radeon_emit(cmd_buffer->cs, dword4);
	radeon_emit(cmd_buffer->cs, dword5);
	radeon_emit(cmd_buffer->cs, encode_tile_info(cmd_buffer, til_image, til_info->mip_level, true));
	radeon_emit(cmd_buffer->cs, lin_info->va);
	radeon_emit(cmd_buffer->cs, lin_info->va >> 32);
	radeon_emit(cmd_buffer->cs, lin_info->offset.x | (lin_info->offset.y << 16));
	radeon_emit(cmd_buffer->cs, lin_info->offset.z | ((lin_info->pitch - 1) << 16));
	radeon_emit(cmd_buffer->cs, lin_info->slice_pitch - 1);
	if (cmd_buffer->device->physical_device->rad_info.chip_class == GFX7) {
		radeon_emit(cmd_buffer->cs, copy_width_aligned | (copy_height << 16));
		radeon_emit(cmd_buffer->cs, copy_depth);
	} else {
		radeon_emit(cmd_buffer->cs, (copy_width_aligned - 1) | ((copy_height - 1) << 16));
		radeon_emit(cmd_buffer->cs, (copy_depth - 1));
	}
}

static void
radv_sdma_copy_image_lin_to_tiled_cik(struct radv_cmd_buffer *cmd_buffer,
				      const struct radv_transfer_image_info *info,
				      struct radv_image *src_image,
				      struct radv_image *dst_image)
{
	const struct radv_transfer_per_image_info *lin_info, *til_info;
	struct radv_image *lin_image, *til_image;
	uint32_t dword0, dword4, dword5;
	unsigned lin_width, til_width;
	bool src_is_linear = src_image->planes[0].surface.is_linear;

	lin_info = src_is_linear ? &info->src_info : &info->dst_info;
	til_info = src_is_linear ? &info->dst_info : &info->src_info;

	lin_image = src_is_linear ? src_image : dst_image;
	til_image = src_is_linear ? dst_image : src_image;

	lin_width = minify_as_blocks(lin_image->info.width,
				     lin_info->mip_level, lin_image->planes[0].surface.blk_w);
	til_width = minify_as_blocks(til_image->info.width,
				     til_info->mip_level, til_image->planes[0].surface.blk_w);
	assert(til_info->pitch % 8 == 0);
	assert(til_info->slice_pitch % 64 == 0);
	unsigned bpp = lin_info->bpp;
	unsigned xalign = MAX2(1, 4 / lin_info->bpp);
	unsigned copy_width = DIV_ROUND_UP(info->extent.width, til_image->planes[0].surface.blk_w);
	unsigned copy_height = DIV_ROUND_UP(info->extent.height, til_image->planes[0].surface.blk_h);
	unsigned copy_width_aligned = copy_width;
	unsigned copy_depth = info->extent.depth;

	/* If the region ends at the last pixel and is unaligned, we
	 * can copy the remainder of the line that is not visible to
	 * make it aligned.
	 */
	if (copy_width % xalign != 0 &&
	    lin_info->offset.x + copy_width == lin_width &&
	    lin_info->offset.x  + copy_width == til_width &&
	    lin_info->offset.x + align(copy_width, xalign) <= lin_info->pitch &&
	    til_info->offset.x  + align(copy_width, xalign) <= til_info->pitch)
		copy_width_aligned = align(copy_width, xalign);

	/* TODO HW Limitations - how do we handle those in vk? */

	bool ret = linear_buffer_workaround(cmd_buffer,
					    til_image, til_info,
					    lin_image, lin_info,
					    copy_width, copy_height, copy_depth,
					    bpp);

	if (ret == false) {
		cmd_buffer->record_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
		return;
	}

	dword0 = CIK_SDMA_PACKET(CIK_SDMA_OPCODE_COPY,
				 CIK_SDMA_COPY_SUB_OPCODE_TILED_SUB_WINDOW, 0) |
		(src_is_linear ? 0 : (1u << 31));

	dword4 = til_info->offset.z;
	unsigned pitch_tile_max = til_info->pitch / 8 - 1;
	unsigned slice_tile_max = til_info->slice_pitch / 64 - 1;

	dword4 |= (pitch_tile_max << 16);
	dword5 = slice_tile_max;

	int num_x_xfers = 1, num_y_xfers = 1;
	if (copy_width_aligned == CIK_MAX_DIM) {
		copy_width_aligned -= 8;
		num_x_xfers++;
//		copy_width_aligned /= 2;
	}
	if ((til_info->offset.y + copy_height == CIK_MAX_DIM) && (copy_height > 1)) {
		num_y_xfers++;
		copy_height -= 1;
	}
	for (uint32_t x = 0; x < num_x_xfers; x++) {
		for (uint32_t y = 0; y < num_y_xfers; y++) {
			uint32_t til_xy, lin_xy;

			til_xy = til_info->offset.x + (x * copy_width_aligned);
			til_xy |= (til_info->offset.y + (y * copy_height)) << 16;

			lin_xy = lin_info->offset.x;// + (x * copy_width_aligned);
			lin_xy |= (lin_info->offset.y << 16);// + (y * copy_height)) << 16;
			radeon_check_space(cmd_buffer->device->ws, cmd_buffer->cs, 14);
			radeon_emit(cmd_buffer->cs, dword0);
			radeon_emit(cmd_buffer->cs, til_info->va);
			radeon_emit(cmd_buffer->cs, til_info->va >> 32);
			radeon_emit(cmd_buffer->cs, til_xy);
			radeon_emit(cmd_buffer->cs, dword4);
			radeon_emit(cmd_buffer->cs, dword5);
			radeon_emit(cmd_buffer->cs, encode_tile_info(cmd_buffer, til_image, til_info->mip_level, true));
			radeon_emit(cmd_buffer->cs, lin_info->va);
			radeon_emit(cmd_buffer->cs, lin_info->va >> 32);
			radeon_emit(cmd_buffer->cs, lin_xy);
			radeon_emit(cmd_buffer->cs, lin_info->offset.z | ((lin_info->pitch - 1) << 16));
			radeon_emit(cmd_buffer->cs, lin_info->slice_pitch - 1);
			radeon_emit(cmd_buffer->cs, ((x == 0) ? copy_width_aligned : 8) | (((y == 0) ? copy_height : 1) << 16));
			radeon_emit(cmd_buffer->cs, copy_depth);
		}
	}
}

static void
radv_sdma_copy_image_tiled(struct radv_cmd_buffer *cmd_buffer,
			   const struct radv_transfer_image_info *info,
			   struct radv_image *src_image,
			   struct radv_image *dst_image)
{
	unsigned dst_width = minify_as_blocks(dst_image->info.width,
					      info->dst_info.mip_level, dst_image->planes[0].surface.blk_w);
	unsigned src_width = minify_as_blocks(src_image->info.width,
					      info->src_info.mip_level, src_image->planes[0].surface.blk_w);
	unsigned dst_height = minify_as_blocks(dst_image->info.height,
					       info->dst_info.mip_level, dst_image->planes[0].surface.blk_h);
	unsigned src_height = minify_as_blocks(src_image->info.height,
					       info->src_info.mip_level, src_image->planes[0].surface.blk_h);

	radeon_check_space(cmd_buffer->device->ws, cmd_buffer->cs, 15);

	unsigned copy_width = DIV_ROUND_UP(info->extent.width, src_image->planes[0].surface.blk_w);
	unsigned copy_height = DIV_ROUND_UP(info->extent.height, src_image->planes[0].surface.blk_h);

	unsigned copy_width_aligned = copy_width;
	unsigned copy_height_aligned = copy_height;
	unsigned dword4, dword5, dword10, dword11;

	if (copy_width % 8 != 0 &&
	    info->src_info.offset.x + copy_width == src_width &&
	    info->dst_info.offset.x + copy_width == dst_width)
		copy_width_aligned = align(copy_width, 8);

	if (copy_height % 8 != 0 &&
	    info->src_info.offset.y + copy_height == src_height &&
	    info->dst_info.offset.y + copy_height == dst_height)
		copy_height_aligned = align(copy_height, 8);

	dword4 = info->src_info.offset.z;
	dword10 = info->dst_info.offset.z;
	if (cmd_buffer->device->physical_device->rad_info.chip_class >= GFX9) {
		dword4 |= (src_image->info.width - 1) << 16;
		dword5 = (src_image->info.height - 1) | ((src_image->info.depth - 1) << 16);

		dword10 |= (dst_image->info.width - 1) << 16;
		dword11 = (dst_image->info.height - 1) | ((dst_image->info.depth - 1) << 16);
	} else {
		unsigned src_pitch_tile_max = info->src_info.pitch / 8 - 1;
		unsigned src_slice_tile_max = info->src_info.slice_pitch / 64 - 1;
		unsigned dst_pitch_tile_max = info->dst_info.pitch / 8 - 1;
		unsigned dst_slice_tile_max = info->dst_info.slice_pitch / 64 - 1;

		dword4 |= (src_pitch_tile_max << 16);
		dword5 = src_slice_tile_max;

		dword10 |= (dst_pitch_tile_max << 16);
		dword11 = dst_slice_tile_max;
	}

	radeon_emit(cmd_buffer->cs, CIK_SDMA_PACKET(CIK_SDMA_OPCODE_COPY,
					CIK_SDMA_COPY_SUB_OPCODE_T2T_SUB_WINDOW, 0));
	radeon_emit(cmd_buffer->cs, info->src_info.va);
	radeon_emit(cmd_buffer->cs, info->src_info.va >> 32);
	radeon_emit(cmd_buffer->cs, info->src_info.offset.x | (info->src_info.offset.y << 16));
	radeon_emit(cmd_buffer->cs, dword4);
	radeon_emit(cmd_buffer->cs, dword5);
	radeon_emit(cmd_buffer->cs, encode_tile_info(cmd_buffer, src_image, info->src_info.mip_level, true));
	radeon_emit(cmd_buffer->cs, info->dst_info.va);
	radeon_emit(cmd_buffer->cs, info->dst_info.va >> 32);
	radeon_emit(cmd_buffer->cs, info->dst_info.offset.x | (info->dst_info.offset.y << 16));
	radeon_emit(cmd_buffer->cs, dword10);
	radeon_emit(cmd_buffer->cs, dword11);
	radeon_emit(cmd_buffer->cs, encode_tile_info(cmd_buffer, dst_image, info->dst_info.mip_level, false));
	if (cmd_buffer->device->physical_device->rad_info.chip_class == GFX7) {
		radeon_emit(cmd_buffer->cs, copy_width_aligned | (copy_height_aligned << 16));
		radeon_emit(cmd_buffer->cs, info->extent.depth);
	} else if (cmd_buffer->device->physical_device->rad_info.chip_class >= GFX9) {
		radeon_emit(cmd_buffer->cs, (copy_width_aligned - 1) | ((copy_height_aligned - 1) << 16));
		radeon_emit(cmd_buffer->cs, (info->extent.depth - 1));
	} else {
		radeon_emit(cmd_buffer->cs, (copy_width_aligned - 8) | ((copy_height_aligned - 8) << 16));
		radeon_emit(cmd_buffer->cs, (info->extent.depth - 1));
	}
}

static void
radv_sdma_copy_image_tiled_cik(struct radv_cmd_buffer *cmd_buffer,
			       const struct radv_transfer_image_info *info,
			       struct radv_image *src_image,
			       struct radv_image *dst_image)
{
	unsigned dst_width = minify_as_blocks(dst_image->info.width,
					      info->dst_info.mip_level, dst_image->planes[0].surface.blk_w);
	unsigned src_width = minify_as_blocks(src_image->info.width,
					      info->src_info.mip_level, src_image->planes[0].surface.blk_w);
	unsigned dst_height = minify_as_blocks(dst_image->info.height,
					       info->dst_info.mip_level, dst_image->planes[0].surface.blk_h);
	unsigned src_height = minify_as_blocks(src_image->info.height,
					       info->src_info.mip_level, src_image->planes[0].surface.blk_h);

	unsigned copy_width = DIV_ROUND_UP(info->extent.width, src_image->planes[0].surface.blk_w);
	unsigned copy_height = DIV_ROUND_UP(info->extent.height, src_image->planes[0].surface.blk_h);

	unsigned copy_width_aligned = copy_width;
	unsigned copy_height_aligned = copy_height;
	unsigned dword4, dword5, dword10, dword11;

	if (copy_width % 8 != 0 &&
	    info->src_info.offset.x + copy_width == src_width &&
	    info->dst_info.offset.x + copy_width == dst_width)
		copy_width_aligned = align(copy_width, 8);

	if (copy_height % 8 != 0 &&
	    info->src_info.offset.y + copy_height == src_height &&
	    info->dst_info.offset.y + copy_height == dst_height)
		copy_height_aligned = align(copy_height, 8);

	dword4 = info->src_info.offset.z;
	dword10 = info->dst_info.offset.z;

	unsigned src_pitch_tile_max = info->src_info.pitch / 8 - 1;
	unsigned src_slice_tile_max = info->src_info.slice_pitch / 64 - 1;
	unsigned dst_pitch_tile_max = info->dst_info.pitch / 8 - 1;
	unsigned dst_slice_tile_max = info->dst_info.slice_pitch / 64 - 1;

	dword4 |= (src_pitch_tile_max << 16);
	dword5 = src_slice_tile_max;

	dword10 |= (dst_pitch_tile_max << 16);
	dword11 = dst_slice_tile_max;

	int num_x_xfers = 1, num_y_xfers = 1;
	if (copy_width_aligned == CIK_MAX_DIM) {
		copy_width_aligned--;
	}
	if ((info->dst_info.offset.y + copy_height_aligned == CIK_MAX_DIM) && (copy_height_aligned > 1)) {
		num_y_xfers++;
		copy_height_aligned -= 1;
	}
	for (uint32_t x = 0; x < num_x_xfers; x++) {
		for (uint32_t y = 0; y < num_y_xfers; y++) {
			uint32_t src_xy, dst_xy;

			src_xy = info->src_info.offset.x + (x * copy_width_aligned);
			src_xy |= (info->src_info.offset.y + (y * copy_height_aligned)) << 16;

			dst_xy = info->dst_info.offset.x + (x * copy_width_aligned);
			dst_xy |= (info->dst_info.offset.y + (y * copy_height_aligned)) << 16;

			radeon_check_space(cmd_buffer->device->ws, cmd_buffer->cs, 15);
			radeon_emit(cmd_buffer->cs, CIK_SDMA_PACKET(CIK_SDMA_OPCODE_COPY,
								    CIK_SDMA_COPY_SUB_OPCODE_T2T_SUB_WINDOW, 0));
			radeon_emit(cmd_buffer->cs, info->src_info.va);
			radeon_emit(cmd_buffer->cs, info->src_info.va >> 32);
			radeon_emit(cmd_buffer->cs, src_xy);
			radeon_emit(cmd_buffer->cs, dword4);
			radeon_emit(cmd_buffer->cs, dword5);
			radeon_emit(cmd_buffer->cs, encode_tile_info(cmd_buffer, src_image, info->src_info.mip_level, true));
			radeon_emit(cmd_buffer->cs, info->dst_info.va);
			radeon_emit(cmd_buffer->cs, info->dst_info.va >> 32);
			radeon_emit(cmd_buffer->cs, dst_xy);
			radeon_emit(cmd_buffer->cs, dword10);
			radeon_emit(cmd_buffer->cs, dword11);
			radeon_emit(cmd_buffer->cs, encode_tile_info(cmd_buffer, dst_image, info->dst_info.mip_level, false));
			radeon_emit(cmd_buffer->cs, copy_width_aligned | (((y == 0) ? copy_height_aligned : 1) << 16));
			radeon_emit(cmd_buffer->cs, info->extent.depth);
		}
	}
}

static VkDeviceSize
radv_sdma_emit_copy_buffer(struct radv_cmd_buffer *cmd_buffer,
			   uint64_t src_va,
			   uint64_t dst_va,
			   VkDeviceSize copy_size)
{
	unsigned bytes_to_copy = MIN2(copy_size, CIK_SDMA_COPY_MAX_SIZE);

	/*
	 * If the source and destination are dword aligned and the size is at least one DWORD,
	 * then go ahead and do DWORD copies.
	 * Note that the SDMA microcode makes the switch between byte and DWORD copies automagically,
	 * depending on the addresses being dword aligned and the size being a dword multiple.
	 */
	if (u_is_aligned(dst_va, 4) && u_is_aligned(src_va, 4) &&
	    copy_size >= 4)
		bytes_to_copy = u_align_down_npot_u32(bytes_to_copy, 4);

	radeon_check_space(cmd_buffer->device->ws, cmd_buffer->cs, 7);
	radeon_emit(cmd_buffer->cs, CIK_SDMA_PACKET(CIK_SDMA_OPCODE_COPY,
						    CIK_SDMA_COPY_SUB_OPCODE_LINEAR,
						    0));
	if (cmd_buffer->device->physical_device->rad_info.chip_class >= GFX9)
		radeon_emit(cmd_buffer->cs, bytes_to_copy - 1);
	else
		radeon_emit(cmd_buffer->cs, bytes_to_copy);
	radeon_emit(cmd_buffer->cs, 0);
	radeon_emit(cmd_buffer->cs, src_va);
	radeon_emit(cmd_buffer->cs, src_va >> 32);
	radeon_emit(cmd_buffer->cs, dst_va);
	radeon_emit(cmd_buffer->cs, dst_va >> 32);

	return bytes_to_copy;
}

static void
radv_sdma_emit_update_buffer(struct radv_cmd_buffer *cmd_buffer,
			     uint64_t dst_va,
			     VkDeviceSize data_size,
			     const void *data)
{
	int num_dw = (data_size + 3) / 4;
	const uint32_t *data_dw = data;
	int left_dw = num_dw;
	do {
		int can_dw = cmd_buffer->cs->max_dw - cmd_buffer->cs->cdw - 4;
		int this_dw = MIN2(left_dw, can_dw);

		radeon_check_space(cmd_buffer->device->ws, cmd_buffer->cs, this_dw + 4);
		radeon_emit(cmd_buffer->cs, CIK_SDMA_PACKET(CIK_SDMA_OPCODE_WRITE,
							    SDMA_WRITE_SUB_OPCODE_LINEAR, 0));
		radeon_emit(cmd_buffer->cs, dst_va);
		radeon_emit(cmd_buffer->cs, dst_va >> 32);
		if (cmd_buffer->device->physical_device->rad_info.chip_class >= GFX9)
			radeon_emit(cmd_buffer->cs, this_dw - 1);
		else
			radeon_emit(cmd_buffer->cs, this_dw);
		radeon_emit_array(cmd_buffer->cs, data_dw, this_dw);

		data_dw += this_dw;
		left_dw -= this_dw;
		dst_va += this_dw * sizeof(uint32_t);

		if (left_dw)
			radeon_check_space(cmd_buffer->device->ws, cmd_buffer->cs, left_dw + 4);
	} while (left_dw > 0);
}

static VkDeviceSize
radv_sdma_emit_fill_buffer(struct radv_cmd_buffer *cmd_buffer,
			   uint64_t dst_va,
			   VkDeviceSize fillSize,
			   uint32_t data)
{
	uint32_t size;
	VkDeviceSize max_fill = ((1ul << 22ull) - 1ull) & (~0x3ull);

	size = MIN2(fillSize, max_fill);

	radeon_check_space(cmd_buffer->device->ws, cmd_buffer->cs, 5);
	radeon_emit(cmd_buffer->cs, CIK_SDMA_PACKET(CIK_SDMA_PACKET_CONSTANT_FILL,
						    0, SDMA_CONSTANT_FILL_DWORDS));
	radeon_emit(cmd_buffer->cs, dst_va);
	radeon_emit(cmd_buffer->cs, dst_va >> 32);
	radeon_emit(cmd_buffer->cs, data);
	if (cmd_buffer->device->physical_device->rad_info.chip_class >= GFX9)
		radeon_emit(cmd_buffer->cs, size - 1);
	else
		radeon_emit(cmd_buffer->cs, size);
	return size;
}

static void
radv_sdma_get_per_image_info(struct radv_image *image,
			     bool is_stencil,
			     struct radv_transfer_per_image_info *info)
{
	const struct legacy_surf_level *base_level = get_base_level_info(image, is_stencil,
									 info->mip_level);
	bool lvl_is_2d_surf = base_level->mode == RADEON_SURF_MODE_2D;

	info->va = radv_buffer_get_va(image->bo) + image->offset;

	info->va += base_level->offset;
	info->va |= lvl_is_2d_surf ? (image->planes[0].surface.tile_swizzle << 8) : 0;
	info->pitch = base_level->nblk_x;
	info->slice_pitch = ((base_level->slice_size_dw * 4) / image->planes[0].surface.bpe);
}

static void
radv_gfx9_sdma_get_per_image_info(struct radv_image *image,
				  bool is_stencil,
				  struct radv_transfer_per_image_info *info)
{
	info->va = radv_buffer_get_va(image->bo) + image->offset;
	info->pitch = image->planes[0].surface.u.gfx9.surf_pitch;
	info->slice_pitch = image->planes[0].surface.u.gfx9.surf_slice_size / image->planes[0].surface.bpe;
	if (image->planes[0].surface.is_linear)
		info->va += image->planes[0].surface.u.gfx9.offset[info->mip_level];
}

static void
radv_sdma_emit_nop(struct radv_cmd_buffer *cmd_buffer)
{
	radeon_check_space(cmd_buffer->device->ws, cmd_buffer->cs, 1);
	/* emit 0 for a transfer NOP on CIK+ */
	radeon_emit(cmd_buffer->cs, 0);
}

static bool
radv_sdma_use_scanline_t2t(struct radv_cmd_buffer *cmd_buffer,
			   const struct radv_transfer_image_info *info,
			   struct radv_image *src_image,
			   struct radv_image *dst_image)
{
	struct radeon_info *rad_info = &cmd_buffer->device->physical_device->rad_info;
	unsigned src_tile_index = src_image->planes[0].surface.u.legacy.tiling_index[info->src_info.mip_level];
	unsigned dst_tile_index = dst_image->planes[0].surface.u.legacy.tiling_index[info->dst_info.mip_level];

	/* src x/y, dst x/y, extent w/h must be aligned to 8 pixels,
	   the vulkan API should guarantee this though.*/
	if (src_tile_index != dst_tile_index)
		return true;

        const uint32_t z_alignment_hw_array_mode[] =
        {
            1, // ARRAY_LINEAR_GENERAL
            1, // ARRAY_LINEAR_ALIGNED
            1, // ARRAY_1D_TILED_THIN1
            4, // ARRAY_1D_TILED_THICK
            1, // ARRAY_2D_TILED_THIN1
            1, // ARRAY_PRT_TILED_THIN1__CI__VI
            1, // ARRAY_PRT_2D_TILED_THIN1__CI__VI
            4, // ARRAY_2D_TILED_THICK
            8, // ARRAY_2D_TILED_XTHICK
            4, // ARRAY_PRT_TILED_THICK__CI__VI
            4, // ARRAY_PRT_2D_TILED_THICK__CI__VI
            1, // ARRAY_PRT_3D_TILED_THIN1__CI__VI
            1, // ARRAY_3D_TILED_THIN1
            4, // ARRAY_3D_TILED_THICK
            8, // ARRAY_3D_TILED_XTHICK
            4, // ARRAY_PRT_3D_TILED_THICK__CI__VI
        };

	unsigned src_tile_mode = rad_info->si_tile_mode_array[src_tile_index];
	src_tile_mode = G_009910_ARRAY_MODE(src_tile_mode);
	const uint32_t z_alignment = z_alignment_hw_array_mode[src_tile_mode];

	/* check Z alignment is correct */
	/* tiletype isn't THICK_MICRO_TILING__CI_VI */
	if (u_is_aligned(info->src_info.offset.z, z_alignment) &&
	    u_is_aligned(info->dst_info.offset.z, z_alignment) &&
	    u_is_aligned(info->extent.depth, z_alignment))
		return false;

	return true;
}

static bool
radv_sdma_use_scanline_t2t_gfx9(struct radv_cmd_buffer *cmd_buffer,
				const struct radv_transfer_image_info *image_info,
				struct radv_image *src_image,
				struct radv_image *dst_image)
{
	/*
	 * The built in tiled-to-tiled image copy packet not only doesn't support mip level selection, it doesn't
	 * even support specifying the number of mip levels the image has.  So if either the source or the destination
	 * image has more than one mip level, we can't use it.
	 */
	if (src_image->info.levels > 1 || dst_image->info.levels > 1)
		return true;
	/* for now always use scanline t2t on gfx9 */
	return true;
}

const static struct radv_transfer_fns sdma20_fns = {
	.emit_copy_buffer = radv_sdma_emit_copy_buffer,
	.emit_update_buffer = radv_sdma_emit_update_buffer,
	.emit_fill_buffer = radv_sdma_emit_fill_buffer,
	.copy_buffer_image_l2l = radv_sdma_copy_one_lin_to_lin_cik,
	.copy_buffer_image_l2t = radv_sdma_copy_one_lin_to_tiled_cik,
	.copy_image_l2l = radv_sdma_copy_image_lin_to_lin_cik,
	.copy_image_l2t = radv_sdma_copy_image_lin_to_tiled_cik,
	.copy_image_t2t = radv_sdma_copy_image_tiled_cik,

	.emit_nop = radv_sdma_emit_nop,
	.get_per_image_info = radv_sdma_get_per_image_info,

	.use_scanline_t2t = radv_sdma_use_scanline_t2t,
};

const static struct radv_transfer_fns sdma24_fns = {
	.emit_copy_buffer = radv_sdma_emit_copy_buffer,
	.emit_update_buffer = radv_sdma_emit_update_buffer,
	.emit_fill_buffer = radv_sdma_emit_fill_buffer,
	.copy_buffer_image_l2l = radv_sdma_copy_one_lin_to_lin,
	.copy_buffer_image_l2t = radv_sdma_copy_one_lin_to_tiled,
	.copy_image_l2l = radv_sdma_copy_image_lin_to_lin,
	.copy_image_l2t = radv_sdma_copy_image_lin_to_tiled,
	.copy_image_t2t = radv_sdma_copy_image_tiled,
	.emit_nop = radv_sdma_emit_nop,
	.get_per_image_info = radv_sdma_get_per_image_info,

	.use_scanline_t2t = radv_sdma_use_scanline_t2t,
};

const static struct radv_transfer_fns sdma40_fns = {
	.emit_copy_buffer = radv_sdma_emit_copy_buffer,
	.emit_update_buffer = radv_sdma_emit_update_buffer,
	.emit_fill_buffer = radv_sdma_emit_fill_buffer,
	.copy_buffer_image_l2l = radv_sdma_copy_one_lin_to_lin,
	.copy_buffer_image_l2t = radv_sdma_copy_one_lin_to_tiled,
	.copy_image_l2l = radv_sdma_copy_image_lin_to_lin,
	.copy_image_l2t = radv_sdma_copy_image_lin_to_tiled,
	.copy_image_t2t = radv_sdma_copy_image_tiled,
	.emit_nop = radv_sdma_emit_nop,
	.get_per_image_info = radv_gfx9_sdma_get_per_image_info,

	.use_scanline_t2t = radv_sdma_use_scanline_t2t_gfx9,
};

static VkDeviceSize
radv_sdma_emit_fill_buffer_si(struct radv_cmd_buffer *cmd_buffer,
			      uint64_t dst_va,
			      VkDeviceSize fillSize,
			      uint32_t data)
{
	VkDeviceSize max_fill = ((1ul << 20ull) - 1ull);

	uint32_t size = MIN2(fillSize, max_fill * sizeof(uint32_t));
	radeon_check_space(cmd_buffer->device->ws, cmd_buffer->cs, 4);
	radeon_emit(cmd_buffer->cs, SI_DMA_PACKET(SI_DMA_PACKET_CONSTANT_FILL, 0, size / sizeof(uint32_t)));
	radeon_emit(cmd_buffer->cs, dst_va);
	radeon_emit(cmd_buffer->cs, data);
	radeon_emit(cmd_buffer->cs, ((dst_va >> 32) & 0xff) << 16);
	return size;
}

static VkDeviceSize
radv_sdma_emit_copy_buffer_si(struct radv_cmd_buffer *cmd_buffer,
			      uint64_t src_va,
			      uint64_t dst_va,
			      VkDeviceSize copy_size)
{
	const VkDeviceSize maxTransferSize = (1ull << 20) - 1 - ((src_va & 0x1C) >> 2);
	VkDeviceSize bytes_to_copy;
	bool use_dword = false;

	/*
	 * If the source and destination are dword aligned and the size is at least one DWORD,
	 * then go ahead and do DWORD copies.
	 * Note that the SDMA microcode makes the switch between byte and DWORD copies automagically,
	 * depending on the addresses being dword aligned and the size being a dword multiple.
	 */
	if (u_is_aligned(dst_va, 4) && u_is_aligned(src_va, 4) &&
	    copy_size >= 4) {
		use_dword = true;
	}

	if (use_dword) {
		VkDeviceSize dwords_to_copy = MIN2(copy_size / sizeof(uint32_t), maxTransferSize);
		bytes_to_copy = dwords_to_copy * sizeof(uint32_t);

		radeon_check_space(cmd_buffer->device->ws, cmd_buffer->cs, 5);
		radeon_emit(cmd_buffer->cs, SI_DMA_PACKET(SI_DMA_PACKET_COPY,
							  0, bytes_to_copy / sizeof(uint32_t)));
		radeon_emit(cmd_buffer->cs, dst_va);
		radeon_emit(cmd_buffer->cs, src_va);
		radeon_emit(cmd_buffer->cs, dst_va >> 32);
		radeon_emit(cmd_buffer->cs, src_va >> 32);
	} else {
		bytes_to_copy = MIN2(copy_size, maxTransferSize);
		radeon_check_space(cmd_buffer->device->ws, cmd_buffer->cs, 5);
		radeon_emit(cmd_buffer->cs, SI_DMA_PACKET(SI_DMA_PACKET_COPY, SI_DMA_COPY_BYTE_ALIGNED,
							  bytes_to_copy));
		radeon_emit(cmd_buffer->cs, dst_va);
		radeon_emit(cmd_buffer->cs, src_va);
		radeon_emit(cmd_buffer->cs, dst_va >> 32);
		radeon_emit(cmd_buffer->cs, src_va >> 32);
	}
	return bytes_to_copy;
}

static void
radv_sdma_emit_update_buffer_si(struct radv_cmd_buffer *cmd_buffer,
				uint64_t dst_va,
				VkDeviceSize data_size,
				const void *data)
{
	int num_dw = (data_size + 3) / 4;
	const uint32_t *data_dw = data;
	int left_dw = num_dw;
	do {
		int can_dw = cmd_buffer->cs->max_dw - cmd_buffer->cs->cdw - 3;
		int this_dw = MIN2(left_dw, can_dw);

		radeon_check_space(cmd_buffer->device->ws, cmd_buffer->cs, this_dw + 3);
		radeon_emit(cmd_buffer->cs, SI_DMA_PACKET(SI_DMA_PACKET_WRITE, 0, this_dw));
		radeon_emit(cmd_buffer->cs, dst_va);
		radeon_emit(cmd_buffer->cs, ((dst_va >> 32) & 0xff));
		radeon_emit_array(cmd_buffer->cs, data_dw, this_dw);
		data_dw += this_dw;
		left_dw -= this_dw;
		dst_va += this_dw * sizeof(uint32_t);
		if (left_dw)
			radeon_check_space(cmd_buffer->device->ws, cmd_buffer->cs, left_dw + 3);
	} while (left_dw > 0);
}

static void
radv_sdma_emit_nop_si(struct radv_cmd_buffer *cmd_buffer)
{
	radeon_check_space(cmd_buffer->device->ws, cmd_buffer->cs, 1);
	/* emit 0 for a transfer NOP on CIK+ */
	radeon_emit(cmd_buffer->cs, SI_DMA_PACKET(SI_DMA_PACKET_NOP, 0, 0));
}

/*
 * Returns the base multiple, in terms of pixels, that doesn't work for the specified bytes-per-pixel value.  i.e., copy
 * widths that are a multiple of the returned value need to be broken up into multiple copies.
 *
 * There is a HW bug related to a shift operation.  All the below cases are affected.
 * Psize=1: DX=0x2000
 * Psize=2: DX=any multiple of 0x1000
 * Psize=3: DX=any multiple of 0x800
 * Psize=4: DX=any multiple of 0x400
 * Where "psize" is equal to log2(bytes-per-pixel)
 */
static int
si_calc_bad_mod_value(uint32_t bpp)
{
    return (0x4000 >> util_logbase2(bpp));
}

static void
si_get_next_extent_and_offset(VkExtent3D orig_extent, VkOffset3D orig_offset,
			      uint32_t bpp, int total_width_copied,
			      VkExtent3D *next_extent, VkOffset3D *next_offset)

{
	*next_extent = orig_extent;
	*next_offset = orig_offset;

	int remaining_width = orig_extent.width - total_width_copied;
	if ((remaining_width % si_calc_bad_mod_value(bpp)) != 0)
		next_extent->width = remaining_width;
	else
		next_extent->width = remaining_width - 8;
	next_offset->x = orig_offset.x + total_width_copied;
}

#define SI_DMA_PACKET_COPY_LINEAR_PARTIAL 0x41
#define SI_DMA_PACKET_COPY_L2TT2L_PARTIAL 0x49
#define SI_DMA_PACKET_COPY_T2T_PARTIAL 0x4d

static uint64_t si_calc_linear_base_addr(const struct radv_transfer_per_image_info *img_info,
					 VkOffset3D offset)
{
	return img_info->va + (offset.z * img_info->slice_pitch * img_info->bpp) +
		(offset.y * img_info->pitch * img_info->bpp) +
		(offset.x * img_info->bpp);
}

static bool
si_check_buffer_info_limits(const struct radv_transfer_image_buffer_info *info, VkExtent3D *extent_out)
{
	*extent_out = info->extent;
	if (info->image_info.offset.y + info->extent.height == CIK_MAX_DIM) {
		fprintf(stderr, "radv: SI hardware bug workaround - not copying last line of 16k image.\n");
		extent_out->height -= 1;
		if (extent_out->height == 0)
			return false;
	}
	return true;
}

static bool
si_check_image_info_limits(const struct radv_transfer_image_info *info, VkExtent3D *extent_out)
{
	*extent_out = info->extent;
	if (info->src_info.offset.y + info->extent.height == CIK_MAX_DIM ||
	    info->dst_info.offset.y + info->extent.height == CIK_MAX_DIM) {
		fprintf(stderr, "radv: SI hardware bug workaround - not copying last line of 16k image.\n");
		extent_out->height -= 1;
		if (extent_out->height == 0)
			return false;
	}
	return true;
}

static void
radv_sdma_copy_one_lin_to_lin_si(struct radv_cmd_buffer *cmd_buffer,
				 const struct radv_transfer_image_buffer_info *info,
				 bool buf2img)
{
	uint32_t total_width_copied = 0;
	uint32_t src_pitch, dst_pitch, src_slice_pitch, dst_slice_pitch;

	src_pitch = buf2img ? info->image_info.pitch : info->buf_info.pitch;
	dst_pitch = buf2img ? info->buf_info.pitch : info->image_info.pitch;

	src_slice_pitch = buf2img ? info->image_info.slice_pitch : info->buf_info.slice_pitch;
	dst_slice_pitch = buf2img ? info->buf_info.slice_pitch : info->image_info.slice_pitch;

	VkExtent3D adjusted_extent;

	if (!si_check_buffer_info_limits(info, &adjusted_extent))
		return;

	while (total_width_copied < adjusted_extent.width) {
		VkExtent3D next_extent;
		VkOffset3D next_offset;

		si_get_next_extent_and_offset(adjusted_extent,
					      info->image_info.offset,
					      info->image_info.bpp,
					      total_width_copied,
					      &next_extent,
					      &next_offset);

		uint64_t this_img_va = si_calc_linear_base_addr(&info->image_info, next_offset);
		uint64_t this_buf_va = info->buf_info.va + total_width_copied * info->image_info.bpp;

		uint64_t this_src_va = buf2img ? this_buf_va : this_img_va;
		uint64_t this_dst_va = buf2img ? this_img_va : this_buf_va;

		radeon_check_space(cmd_buffer->device->ws, cmd_buffer->cs, 9);
		radeon_emit(cmd_buffer->cs, SI_DMA_PACKET(SI_DMA_PACKET_COPY, SI_DMA_PACKET_COPY_LINEAR_PARTIAL, 0));
		radeon_emit(cmd_buffer->cs, this_src_va);
		radeon_emit(cmd_buffer->cs, ((this_src_va >> 32) & 0xff) | (src_pitch << 13));
		radeon_emit(cmd_buffer->cs, src_slice_pitch);
		radeon_emit(cmd_buffer->cs, this_dst_va);
		radeon_emit(cmd_buffer->cs, (this_dst_va >> 32 & 0xff) | (dst_pitch << 13));
		radeon_emit(cmd_buffer->cs, dst_slice_pitch);
		radeon_emit(cmd_buffer->cs, next_extent.width | (next_extent.height << 16));/* sizeXY */
		radeon_emit(cmd_buffer->cs, next_extent.depth | (info->image_info.bpp << 29));/* sizeZ */

		total_width_copied += next_extent.width;
	}

}
static void
radv_sdma_copy_one_lin_to_tiled_si(struct radv_cmd_buffer *cmd_buffer,
				   const struct radv_transfer_image_buffer_info *info,
				   struct radv_image *image,
				   bool buf2img)
{
	struct radeon_info *rad_info = &cmd_buffer->device->physical_device->rad_info;
	unsigned index = image->planes[0].surface.u.legacy.tiling_index[info->image_info.mip_level];
	unsigned tile_mode = rad_info->si_tile_mode_array[index];
	uint32_t total_width_copied = 0;

	uint32_t array_mode = G_009910_ARRAY_MODE(tile_mode);
	uint32_t bank_h = G_009910_BANK_HEIGHT(tile_mode);
	uint32_t bank_w = G_009910_BANK_WIDTH(tile_mode);
	uint32_t mt_aspect = G_009910_MACRO_TILE_ASPECT(tile_mode);
	uint32_t pipe_config = G_009910_PIPE_CONFIG(tile_mode);
	uint32_t mt = G_009910_MICRO_TILE_MODE(tile_mode);
	uint32_t nbanks = G_009910_NUM_BANKS(tile_mode);
	uint32_t pitch_tile_max = info->image_info.pitch / 8 - 1;
	uint64_t slice_tile_max = info->image_info.slice_pitch / 64 - 1;
	uint32_t tile_split = util_logbase2(image->planes[0].surface.u.legacy.tile_split >> 6);
	uint32_t height = radv_minify(image->info.height, info->image_info.mip_level);

	VkExtent3D adjusted_extent;
	if (!si_check_buffer_info_limits(info, &adjusted_extent))
		return;

	while (total_width_copied < info->extent.width) {
		VkExtent3D next_extent;
		VkOffset3D next_offset;

		uint64_t this_lin_va = info->buf_info.va + (total_width_copied * info->image_info.bpp);
		uint32_t tile_info0, tile_info4;
		si_get_next_extent_and_offset(adjusted_extent,
					      info->image_info.offset,
					      info->image_info.bpp,
					      total_width_copied,
					      &next_extent,
					      &next_offset);
		radeon_check_space(cmd_buffer->device->ws, cmd_buffer->cs, 12);

		tile_info0 = buf2img ? 0 : (1 << 31);
		tile_info0 |= util_logbase2(info->image_info.bpp) << 24;
		tile_info0 |= (array_mode << 27) | (bank_h << 21) | (bank_w << 18);
		tile_info0 |= (mt_aspect << 16);

		tile_info4 = next_offset.y;
		tile_info4 |= (tile_split << 21) | (nbanks << 25) | (mt << 27);

		radeon_emit(cmd_buffer->cs, SI_DMA_PACKET(SI_DMA_PACKET_COPY, SI_DMA_PACKET_COPY_L2TT2L_PARTIAL, 0));
		radeon_emit(cmd_buffer->cs, info->image_info.va >> 8);
		radeon_emit(cmd_buffer->cs, tile_info0);
		radeon_emit(cmd_buffer->cs, pitch_tile_max | (((height - 1) << 16)));
		radeon_emit(cmd_buffer->cs, slice_tile_max | (pipe_config << 26));
		radeon_emit(cmd_buffer->cs, next_offset.x | (next_offset.z << 18));
		radeon_emit(cmd_buffer->cs, tile_info4);
		radeon_emit(cmd_buffer->cs, this_lin_va & 0xfffffffc);
		radeon_emit(cmd_buffer->cs, ((this_lin_va >> 32) & 0xff) | ((info->buf_info.pitch * info->image_info.bpp) << 13));
		radeon_emit(cmd_buffer->cs, info->buf_info.slice_pitch * info->image_info.bpp);
		radeon_emit(cmd_buffer->cs, next_extent.width | (next_extent.height << 16));
		radeon_emit(cmd_buffer->cs, next_extent.depth);

		total_width_copied += next_extent.width;
	}
}

static void
radv_sdma_copy_image_lin_to_lin_si(struct radv_cmd_buffer *cmd_buffer,
				   const struct radv_transfer_image_info *info,
				   struct radv_image *src_image,
				   struct radv_image *dst_image)
{
	uint32_t total_width_copied = 0;
	VkExtent3D adjusted_extent;

	if (!si_check_image_info_limits(info, &adjusted_extent))
		return;
	while (total_width_copied < info->extent.width) {
		VkExtent3D next_extent;
		VkOffset3D next_offset;

		si_get_next_extent_and_offset(adjusted_extent,
					      info->src_info.offset,
					      info->src_info.bpp,
					      total_width_copied,
					      &next_extent,
					      &next_offset);

		si_get_next_extent_and_offset(adjusted_extent,
					      info->dst_info.offset,
					      info->dst_info.bpp,
					      total_width_copied,
					      &next_extent,
					      &next_offset);

		uint64_t this_src_va = si_calc_linear_base_addr(&info->src_info, next_offset);
		uint64_t this_dst_va = si_calc_linear_base_addr(&info->dst_info, next_offset);

		radeon_check_space(cmd_buffer->device->ws, cmd_buffer->cs, 9);
		radeon_emit(cmd_buffer->cs, SI_DMA_PACKET(SI_DMA_PACKET_COPY, SI_DMA_PACKET_COPY_LINEAR_PARTIAL, 0));
		radeon_emit(cmd_buffer->cs, this_src_va);
		radeon_emit(cmd_buffer->cs, ((this_src_va >> 32) & 0xff) | ((info->src_info.pitch * info->src_info.bpp) << 13));
		radeon_emit(cmd_buffer->cs, info->src_info.slice_pitch * info->src_info.bpp);
		radeon_emit(cmd_buffer->cs, this_dst_va);
		radeon_emit(cmd_buffer->cs, (this_dst_va >> 32 & 0xff) | ((info->dst_info.pitch * info->dst_info.bpp) << 13));
		radeon_emit(cmd_buffer->cs, info->dst_info.slice_pitch * info->dst_info.bpp);
		radeon_emit(cmd_buffer->cs, next_extent.width | (next_extent.height << 16));/* sizeXY */
		radeon_emit(cmd_buffer->cs, next_extent.depth | (util_logbase2(info->dst_info.bpp) << 29));/* sizeZ */

		total_width_copied += next_extent.width;
	}
}

static void
radv_sdma_copy_image_lin_to_tiled_si(struct radv_cmd_buffer *cmd_buffer,
				     const struct radv_transfer_image_info *info,
				     struct radv_image *src_image,
				     struct radv_image *dst_image)
{
	struct radeon_info *rad_info = &cmd_buffer->device->physical_device->rad_info;
	const struct radv_transfer_per_image_info *lin_info, *til_info;
	struct radv_image *til_image;
	bool src_is_linear = src_image->planes[0].surface.is_linear;

	lin_info = src_is_linear ? &info->src_info : &info->dst_info;
	til_info = src_is_linear ? &info->dst_info : &info->src_info;

	til_image = src_is_linear ? dst_image : src_image;

	unsigned index = til_image->planes[0].surface.u.legacy.tiling_index[til_info->mip_level];
	unsigned tile_mode = rad_info->si_tile_mode_array[index];
	uint32_t total_width_copied = 0;

	uint32_t array_mode = G_009910_ARRAY_MODE(tile_mode);
	uint32_t bank_h = G_009910_BANK_HEIGHT(tile_mode);
	uint32_t bank_w = G_009910_BANK_WIDTH(tile_mode);
	uint32_t mt_aspect = G_009910_MACRO_TILE_ASPECT(tile_mode);
	uint32_t pipe_config = G_009910_PIPE_CONFIG(tile_mode);
	uint32_t mt = G_009910_MICRO_TILE_MODE(tile_mode);
	uint32_t nbanks = G_009910_NUM_BANKS(tile_mode);
	uint32_t pitch_tile_max = til_info->pitch / 8 - 1;
	uint64_t slice_tile_max = til_info->slice_pitch / 64 - 1;
	uint32_t tile_split = util_logbase2(til_image->planes[0].surface.u.legacy.tile_split >> 6);
	uint32_t height = radv_minify(til_image->info.height, til_info->mip_level);
	VkExtent3D adjusted_extent;

	if (!si_check_image_info_limits(info, &adjusted_extent))
		return;

	while (total_width_copied < adjusted_extent.width) {
		VkExtent3D next_extent;
		VkOffset3D next_offset;
		uint32_t tile_info0, tile_info4;

		si_get_next_extent_and_offset(adjusted_extent,
					      lin_info->offset,
					      lin_info->bpp,
					      total_width_copied,
					      &next_extent,
					      &next_offset);
		si_get_next_extent_and_offset(adjusted_extent,
					      til_info->offset,
					      til_info->bpp,
					      total_width_copied,
					      &next_extent,
					      &next_offset);

		uint64_t this_lin_va = si_calc_linear_base_addr(lin_info, next_offset);
		radeon_check_space(cmd_buffer->device->ws, cmd_buffer->cs, 12);

		tile_info0 = src_is_linear ? 0 : (1 << 31);
		tile_info0 |= util_logbase2(til_info->bpp) << 24;
		tile_info0 |= (array_mode << 27) | (bank_h << 21) | (bank_w << 18);
		tile_info0 |= (mt_aspect << 16);

		tile_info4 = next_offset.y;
		tile_info4 |= (tile_split << 21) | (nbanks << 25) | (mt << 27);

		radeon_emit(cmd_buffer->cs, SI_DMA_PACKET(SI_DMA_PACKET_COPY, SI_DMA_PACKET_COPY_L2TT2L_PARTIAL, 0));
		radeon_emit(cmd_buffer->cs, til_info->va >> 8);
		radeon_emit(cmd_buffer->cs, tile_info0);
		radeon_emit(cmd_buffer->cs, pitch_tile_max | (((height - 1) << 16)));
		radeon_emit(cmd_buffer->cs, slice_tile_max | (pipe_config << 26));
		radeon_emit(cmd_buffer->cs, next_offset.x | (next_offset.z << 18));
		radeon_emit(cmd_buffer->cs, tile_info4);
		radeon_emit(cmd_buffer->cs, this_lin_va & 0xfffffffc);
		radeon_emit(cmd_buffer->cs, ((this_lin_va >> 32) & 0xff) | ((lin_info->pitch * lin_info->bpp) << 13));
		radeon_emit(cmd_buffer->cs, lin_info->slice_pitch * lin_info->bpp);
		radeon_emit(cmd_buffer->cs, next_extent.width | (next_extent.height << 16));
		radeon_emit(cmd_buffer->cs, next_extent.depth);

		total_width_copied += next_extent.width;
	}
}

static void
radv_sdma_copy_image_tiled_si(struct radv_cmd_buffer *cmd_buffer,
			      const struct radv_transfer_image_info *info,
			      struct radv_image *src_image,
			      struct radv_image *dst_image)
{
	struct radeon_info *rad_info = &cmd_buffer->device->physical_device->rad_info;
	uint32_t src_pitch_tile_max = info->src_info.pitch / 8 - 1;
	uint64_t src_slice_tile_max = info->src_info.slice_pitch / 64 - 1;

	uint32_t dst_pitch_tile_max = info->dst_info.pitch / 8 - 1;
	uint64_t dst_slice_tile_max = info->dst_info.slice_pitch / 64 - 1;

	unsigned index = src_image->planes[0].surface.u.legacy.tiling_index[info->src_info.mip_level];
	unsigned tile_mode = rad_info->si_tile_mode_array[index];

	uint32_t array_mode = G_009910_ARRAY_MODE(tile_mode);
	uint32_t bank_h = G_009910_BANK_HEIGHT(tile_mode);
	uint32_t bank_w = G_009910_BANK_WIDTH(tile_mode);
	uint32_t mt_aspect = G_009910_MACRO_TILE_ASPECT(tile_mode);
	uint32_t pipe_config = G_009910_PIPE_CONFIG(tile_mode);
	uint32_t mt = G_009910_MICRO_TILE_MODE(tile_mode);
	uint32_t nbanks = G_009910_NUM_BANKS(tile_mode);
	uint32_t tile_split = util_logbase2(dst_image->planes[0].surface.u.legacy.tile_split >> 6);
	uint32_t info0, xinfo1, yinfo1, zinfo2;

	uint32_t src_height = radv_minify(src_image->info.height, info->src_info.mip_level);
	uint32_t dst_height = radv_minify(dst_image->info.height, info->dst_info.mip_level);
	info0 = (tile_split << 3) |
		(nbanks << 7) |
		(mt << 9) |
		(mt_aspect << 16) |
		(bank_w << 18) |
		(bank_h << 21) |
		(util_logbase2(info->src_info.bpp) << 24) |
		(array_mode << 27);

	VkExtent3D adjusted_extent;

	if (!si_check_image_info_limits(info, &adjusted_extent))
		return;

	uint32_t total_width_copied = 0;
	while (total_width_copied < adjusted_extent.width) {
		VkExtent3D next_extent;
		VkOffset3D next_offset;
		si_get_next_extent_and_offset(adjusted_extent,
					      info->src_info.offset,
					      info->src_info.bpp,
					      total_width_copied,
					      &next_extent,
					      &next_offset);
		xinfo1 = (info->dst_info.offset.x + total_width_copied) | ((info->src_info.offset.x + total_width_copied) << 16);
		yinfo1 = info->dst_info.offset.y | (info->src_info.offset.y << 16);
		zinfo2 = info->dst_info.offset.z | (info->src_info.offset.z << 16);
		radeon_check_space(cmd_buffer->device->ws, cmd_buffer->cs, 13);
		radeon_emit(cmd_buffer->cs, SI_DMA_PACKET(SI_DMA_PACKET_COPY, SI_DMA_PACKET_COPY_T2T_PARTIAL, 0));
		radeon_emit(cmd_buffer->cs, info->src_info.va >> 8);
		radeon_emit(cmd_buffer->cs, src_pitch_tile_max | (src_height - 1) << 16);
		radeon_emit(cmd_buffer->cs, src_slice_tile_max | (pipe_config << 26));

		radeon_emit(cmd_buffer->cs, info->dst_info.va >> 8);
		radeon_emit(cmd_buffer->cs, dst_pitch_tile_max | (dst_height - 1) << 16);
		radeon_emit(cmd_buffer->cs, dst_slice_tile_max | (pipe_config << 26));

		radeon_emit(cmd_buffer->cs, info0);
		radeon_emit(cmd_buffer->cs, xinfo1);
		radeon_emit(cmd_buffer->cs, yinfo1);
		radeon_emit(cmd_buffer->cs, zinfo2);

		radeon_emit(cmd_buffer->cs, next_extent.width | (next_extent.height << 16));
		radeon_emit(cmd_buffer->cs, next_extent.depth);

		total_width_copied += next_extent.width;
	}
}

static bool
radv_sdma_use_scanline_t2t_si(struct radv_cmd_buffer *cmd_buffer,
			      const struct radv_transfer_image_info *info,
			      struct radv_image *src_image,
			      struct radv_image *dst_image)
{
	unsigned src_tile_index = src_image->planes[0].surface.u.legacy.tiling_index[info->src_info.mip_level];
	unsigned dst_tile_index = dst_image->planes[0].surface.u.legacy.tiling_index[info->dst_info.mip_level];

	/* src x/y, dst x/y, extent w/h must be aligned to 8 pixels,
	   the vulkan API should guarantee this though.*/
	if (src_tile_index != dst_tile_index)
		return true;

	/* use scanline copies if we are going to hit the height limit - we'll at least copy the first 16383 lines */
	VkExtent3D adjusted_extent;
	if (!si_check_image_info_limits(info, &adjusted_extent))
		return true;
	if (adjusted_extent.height != info->extent.height)
		return true;
	/* Beyond the documented T2T packet restricitons, there is an apparent hardware bug with OSS 1.0
	 * that causes corruption when copying from a 2D to a 3D image where the source array-slice doesn't
	 * match the destination Z-slice.
	 */
	if (src_image->type == VK_IMAGE_TYPE_2D &&
	    dst_image->type == VK_IMAGE_TYPE_3D &&
	    info->dst_info.offset.z > 0 &&
	    info->dst_info.offset.z != info->src_info.offset.z)
		return true;
	return false;
}

const static struct radv_transfer_fns sdma10_fns = {
	.emit_copy_buffer = radv_sdma_emit_copy_buffer_si,
	.emit_fill_buffer = radv_sdma_emit_fill_buffer_si,
	.emit_update_buffer = radv_sdma_emit_update_buffer_si,

	.copy_buffer_image_l2l = radv_sdma_copy_one_lin_to_lin_si,
	.copy_buffer_image_l2t = radv_sdma_copy_one_lin_to_tiled_si,

	.copy_image_l2l = radv_sdma_copy_image_lin_to_lin_si,
	.copy_image_l2t = radv_sdma_copy_image_lin_to_tiled_si,
	.copy_image_t2t = radv_sdma_copy_image_tiled_si,

	.get_per_image_info = radv_sdma_get_per_image_info,
	.emit_nop = radv_sdma_emit_nop_si,

	.use_scanline_t2t = radv_sdma_use_scanline_t2t_si,
};

void radv_setup_transfer(struct radv_device *device)
{
	if (device->physical_device->rad_info.chip_class == GFX6)
		device->transfer_fns = &sdma10_fns;
	else if (device->physical_device->rad_info.chip_class == GFX7)
		device->transfer_fns = &sdma20_fns;
	else if (device->physical_device->rad_info.chip_class == GFX8)
		device->transfer_fns = &sdma24_fns;
	else if (device->physical_device->rad_info.chip_class == GFX9)
		device->transfer_fns = &sdma40_fns;
}
