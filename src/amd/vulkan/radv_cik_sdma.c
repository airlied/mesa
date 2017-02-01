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
#include "vk_format.h"
#include "radv_cs.h"

static VkFormat get_format_from_aspect_mask(VkImageAspectFlags aspectMask,
					    VkFormat format)
{
	if (aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT)
		format = vk_format_depth_only(format);
	else if (aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT)
		format = vk_format_stencil_only(format);
	return format;
}

static unsigned minify_as_blocks(unsigned width, unsigned level, unsigned blk_w)
{
	width = radv_minify(width, level);
	return DIV_ROUND_UP(width, blk_w);
}

static const struct legacy_surf_level *get_base_level_info(const struct radv_image *img,
							   VkImageAspectFlags aspectMask, int base_mip_level)
{
	if (aspectMask == VK_IMAGE_ASPECT_STENCIL_BIT)
		return &img->surface.u.legacy.stencil_level[base_mip_level];
	return &img->surface.u.legacy.level[base_mip_level];
}

static void get_image_info(struct radv_cmd_buffer *cmd_buffer,
			   const struct radv_image *img,
			   const VkImageSubresourceLayers *subres,
			   uint64_t *va_p, uint32_t *bpp_p, uint32_t *pitch, uint32_t *slice_pitch)
{
	const struct legacy_surf_level *base_level = get_base_level_info(img, subres->aspectMask,
									 subres->mipLevel);
	VkFormat format = get_format_from_aspect_mask(subres->aspectMask, img->vk_format);
	uint32_t bpp = vk_format_get_blocksize(format);
	uint64_t va = radv_buffer_get_va(img->bo);
	bool lvl_is_2d_surf = base_level->mode == RADEON_SURF_MODE_2D;

	va += img->offset;
	va += base_level->offset;
	va |= lvl_is_2d_surf ? (img->surface.tile_swizzle << 8) : 0;
	*pitch = base_level->nblk_x;
	*slice_pitch = (base_level->slice_size_dw * 4) / bpp;
	if (bpp_p)
		*bpp_p = bpp;
	*va_p = va;
}

static unsigned encode_tile_info(struct radv_cmd_buffer *cmd_buffer,
				 struct radv_image *image, unsigned level,
				 bool set_bpp)
{
	struct radeon_info *info = &cmd_buffer->device->physical_device->rad_info;
	unsigned tile_index = image->surface.u.legacy.tiling_index[level];
	unsigned macro_tile_index = image->surface.u.legacy.macro_tile_index;
	unsigned tile_mode = info->si_tile_mode_array[tile_index];
	unsigned macro_tile_mode = info->cik_macrotile_mode_array[macro_tile_index];

	return (set_bpp ? util_logbase2(image->surface.bpe) : 0) |
		(G_009910_ARRAY_MODE(tile_mode) << 3) |
		(G_009910_MICRO_TILE_MODE_NEW(tile_mode) << 8) |
		/* Non-depth modes don't have TILE_SPLIT set. */
		((util_logbase2(image->surface.u.legacy.tile_split >> 6)) << 11) |
		(G_009990_BANK_WIDTH(macro_tile_mode) << 15) |
		(G_009990_BANK_HEIGHT(macro_tile_mode) << 18) |
		(G_009990_NUM_BANKS(macro_tile_mode) << 21) |
		(G_009990_MACRO_TILE_ASPECT(macro_tile_mode) << 24) |
		(G_009910_PIPE_CONFIG(tile_mode) << 26);
}

static void
get_buffer_info(struct radv_cmd_buffer *cmd_buffer,
		const struct radv_buffer *buffer,
		const VkBufferImageCopy *region,
		uint32_t block_width, uint32_t block_height,
		uint64_t *va_p, uint32_t *pitch, uint32_t *slice_pitch)
{
	uint64_t va = radv_buffer_get_va(buffer->bo);
	uint32_t rowLength = region->bufferRowLength;
	uint32_t imageHeight = region->bufferImageHeight;

	va += buffer->offset;
	va += region->bufferOffset;

	*va_p = va;

	if (rowLength == 0)
		rowLength = region->imageExtent.width;

	if (imageHeight == 0)
		imageHeight = region->imageExtent.height;

	*pitch = rowLength / block_width;
	*slice_pitch = *pitch * imageHeight / block_height;
}

static void
get_bufimage_depth_info(VkImageType type,
			const VkBufferImageCopy *region,
			uint32_t *zoffset, uint32_t *depth)
{
	if (type == VK_IMAGE_TYPE_3D) {
		*depth = region->imageExtent.depth;
		*zoffset = region->imageOffset.z;
	} else {
		*depth = region->imageSubresource.layerCount;
		*zoffset = region->imageSubresource.baseArrayLayer;
	}
}

static bool
linear_buffer_workaround(struct radv_cmd_buffer *cmd_buffer,
			 struct radv_image *image,
			 uint32_t level,
			 uint32_t bpp, unsigned *granularity_p)
{
	struct radeon_info *info = &cmd_buffer->device->physical_device->rad_info;
	unsigned til_tile_index = image->surface.u.legacy.tiling_index[level];
	unsigned til_tile_mode = info->si_tile_mode_array[til_tile_index];
	unsigned til_micro_mode = G_009910_MICRO_TILE_MODE_NEW(til_tile_mode);
	unsigned granularity;

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

	*granularity_p = granularity;
	return true;
}

/* L2L buffer->image + image->buffer */
static void
radv_cik_dma_copy_one_lin_to_lin(struct radv_cmd_buffer *cmd_buffer,
				 struct radv_buffer *buffer,
				 struct radv_image *image,
				 const VkBufferImageCopy *region,
				 bool buf2img)
{
	uint64_t buf_va, img_va;
	uint64_t src_va, dst_va;
	unsigned depth;
	unsigned zoffset;
	uint32_t bpp, pitch, slice_pitch;
	unsigned linear_pitch;
	unsigned linear_slice_pitch;

	radeon_check_space(cmd_buffer->device->ws, cmd_buffer->cs, 13);
	get_image_info(cmd_buffer, image, &region->imageSubresource, &img_va,
		       &bpp, &pitch, &slice_pitch);
	get_buffer_info(cmd_buffer, buffer, region, image->surface.blk_w, image->surface.blk_h,
	                &buf_va, &linear_pitch, &linear_slice_pitch);

	get_bufimage_depth_info(image->type, region, &zoffset, &depth);

	src_va = buf2img ? buf_va : img_va;
	dst_va = buf2img ? img_va : buf_va;

	radeon_emit(cmd_buffer->cs, CIK_SDMA_PACKET(CIK_SDMA_OPCODE_COPY,
						    CIK_SDMA_COPY_SUB_OPCODE_LINEAR_SUB_WINDOW, 0) |
		    (util_logbase2(bpp) << 29));
	radeon_emit(cmd_buffer->cs, src_va);
	radeon_emit(cmd_buffer->cs, src_va >> 32);
	radeon_emit(cmd_buffer->cs, 0);
	radeon_emit(cmd_buffer->cs, ((linear_pitch - 1) << 16));
	radeon_emit(cmd_buffer->cs, (linear_slice_pitch - 1));
	radeon_emit(cmd_buffer->cs, dst_va);
	radeon_emit(cmd_buffer->cs, dst_va >> 32);
	radeon_emit(cmd_buffer->cs, region->imageOffset.x | (region->imageOffset.y << 16));
	radeon_emit(cmd_buffer->cs, zoffset | ((pitch - 1) << 16));
	radeon_emit(cmd_buffer->cs, (slice_pitch - 1));
	if (cmd_buffer->device->physical_device->rad_info.chip_class == CIK) {
		radeon_emit(cmd_buffer->cs, region->imageExtent.width | (region->imageExtent.height << 16));
		radeon_emit(cmd_buffer->cs, depth);
	} else {
		radeon_emit(cmd_buffer->cs, (region->imageExtent.width -1) | ((region->imageExtent.height - 1) << 16));
		radeon_emit(cmd_buffer->cs, (depth - 1));
	}
}

/* L2T buffer->image + image->buffer */
static void
radv_cik_dma_copy_one_lin_to_tiled(struct radv_cmd_buffer *cmd_buffer,
				   struct radv_buffer *buffer,
				   struct radv_image *image,
				   const VkBufferImageCopy *region,
				   bool buf2img)
{
	uint64_t buf_va, img_va;
	unsigned depth;
	unsigned zoffset;
	unsigned pitch, slice_pitch;

	radeon_check_space(cmd_buffer->device->ws, cmd_buffer->cs, 14);
	get_image_info(cmd_buffer, image, &region->imageSubresource, &img_va,
		       NULL, &pitch, &slice_pitch);

	unsigned pitch_tile_max = pitch / 8 - 1;
	unsigned slice_tile_max = slice_pitch / 64 - 1;

	unsigned copy_width = DIV_ROUND_UP(region->imageExtent.width, image->surface.blk_w);
	unsigned copy_height = DIV_ROUND_UP(region->imageExtent.height, image->surface.blk_h);
	unsigned copy_width_aligned = copy_width;
	unsigned linear_pitch;
	unsigned linear_slice_pitch;

	get_buffer_info(cmd_buffer, buffer, region, image->surface.blk_w, image->surface.blk_h,
	                &buf_va, &linear_pitch, &linear_slice_pitch);

	get_bufimage_depth_info(image->type, region, &zoffset, &depth);

	radeon_emit(cmd_buffer->cs, CIK_SDMA_PACKET(CIK_SDMA_OPCODE_COPY,
						    CIK_SDMA_COPY_SUB_OPCODE_TILED_SUB_WINDOW, 0) |
		    (buf2img ? 0 : (1u << 31)));
	radeon_emit(cmd_buffer->cs, img_va);
	radeon_emit(cmd_buffer->cs, img_va >> 32);
	radeon_emit(cmd_buffer->cs, region->imageOffset.x | (region->imageOffset.y << 16));
	radeon_emit(cmd_buffer->cs, zoffset | (pitch_tile_max << 16));
	radeon_emit(cmd_buffer->cs, slice_tile_max);
	radeon_emit(cmd_buffer->cs, encode_tile_info(cmd_buffer, image, region->imageSubresource.mipLevel, true));
	radeon_emit(cmd_buffer->cs, buf_va);
	radeon_emit(cmd_buffer->cs, buf_va >> 32);
	radeon_emit(cmd_buffer->cs, 0/*x,y*/);
	radeon_emit(cmd_buffer->cs, ((linear_pitch - 1) << 16));
	radeon_emit(cmd_buffer->cs, linear_slice_pitch - 1);
	if (cmd_buffer->device->physical_device->rad_info.chip_class == CIK) {
		radeon_emit(cmd_buffer->cs, copy_width_aligned | (copy_height << 16));
		radeon_emit(cmd_buffer->cs, depth);
	} else {
		radeon_emit(cmd_buffer->cs, (copy_width_aligned - 1) | ((copy_height - 1) << 16));
		radeon_emit(cmd_buffer->cs, (depth - 1));
	}
}

/* T2T */

void radv_cik_dma_copy_buffer_to_image(struct radv_cmd_buffer *cmd_buffer,
				       struct radv_buffer *src_buffer,
				       struct radv_image *dest_image,
				       uint32_t region_count,
				       const VkBufferImageCopy *pRegions)
{
	uint32_t r;
	for (r = 0; r < region_count; r++) {
		const VkBufferImageCopy *region = &pRegions[r];
		if (dest_image->surface.u.legacy.level[region->imageSubresource.mipLevel].mode == RADEON_SURF_MODE_LINEAR_ALIGNED) {
			/* L -> L  */
			radv_cik_dma_copy_one_lin_to_lin(cmd_buffer, src_buffer, dest_image,
							 region, true);
		} else {
			/* L -> T */
			radv_cik_dma_copy_one_lin_to_tiled(cmd_buffer, src_buffer, dest_image,
							   region, true);
		}
	}
}

void radv_cik_dma_copy_image_to_buffer(struct radv_cmd_buffer *cmd_buffer,
				       struct radv_image *src_image,
				       struct radv_buffer *dest_buffer,
				       uint32_t region_count,
				       const VkBufferImageCopy *pRegions)
{
	uint32_t r;
	for (r = 0; r < region_count; r++) {
		const VkBufferImageCopy *region = &pRegions[r];

		if (src_image->surface.u.legacy.level[region->imageSubresource.mipLevel].mode == RADEON_SURF_MODE_LINEAR_ALIGNED) {
			/* L -> L */
			radv_cik_dma_copy_one_lin_to_lin(cmd_buffer, dest_buffer, src_image,
							 region, false);
		} else {
			/* L -> T */
			radv_cik_dma_copy_one_lin_to_tiled(cmd_buffer, dest_buffer, src_image,
							   region, false);
		}
	}
}

/* L2L buffer->image */
static void
radv_cik_dma_copy_one_image_lin_to_lin(struct radv_cmd_buffer *cmd_buffer,
				       struct radv_image *src_image,
				       struct radv_image *dst_image,
				       const VkImageCopy *region)
{
	uint64_t src_va, dst_va;
	unsigned src_pitch, src_slice_pitch, src_zoffset;
	unsigned dst_pitch, dst_slice_pitch, dst_zoffset;
	unsigned depth;
	unsigned bpp;

	radeon_check_space(cmd_buffer->device->ws, cmd_buffer->cs, 13);
	get_image_info(cmd_buffer, src_image, &region->srcSubresource, &src_va,
		       &bpp, &src_pitch, &src_slice_pitch);
	get_image_info(cmd_buffer, dst_image, &region->dstSubresource, &dst_va,
		       NULL, &dst_pitch, &dst_slice_pitch);

	if (src_image->type == VK_IMAGE_TYPE_3D) {
		depth = region->extent.depth;
		src_zoffset = region->srcOffset.z;
	} else {
		depth = region->srcSubresource.layerCount;
		src_zoffset = region->srcSubresource.baseArrayLayer;
	}

	if (dst_image->type == VK_IMAGE_TYPE_3D) {
		dst_zoffset = region->dstOffset.z;
	} else {
		dst_zoffset = region->dstSubresource.baseArrayLayer;
	}

	radeon_emit(cmd_buffer->cs, CIK_SDMA_PACKET(CIK_SDMA_OPCODE_COPY,
						    CIK_SDMA_COPY_SUB_OPCODE_LINEAR_SUB_WINDOW, 0) |
		    (util_logbase2(bpp) << 29));
	radeon_emit(cmd_buffer->cs, src_va);
	radeon_emit(cmd_buffer->cs, src_va >> 32);
	radeon_emit(cmd_buffer->cs, region->srcOffset.x | (region->srcOffset.y << 16));
	radeon_emit(cmd_buffer->cs, src_zoffset | ((src_pitch - 1) << 16));
	radeon_emit(cmd_buffer->cs, src_slice_pitch - 1);
	radeon_emit(cmd_buffer->cs, dst_va);
	radeon_emit(cmd_buffer->cs, dst_va >> 32);
	radeon_emit(cmd_buffer->cs, region->dstOffset.x | (region->dstOffset.y << 16));
	radeon_emit(cmd_buffer->cs, dst_zoffset | ((dst_pitch - 1) << 16));
	radeon_emit(cmd_buffer->cs, dst_slice_pitch - 1);
	if (cmd_buffer->device->physical_device->rad_info.chip_class == CIK) {
		radeon_emit(cmd_buffer->cs, region->extent.width | (region->extent.height << 16));
		radeon_emit(cmd_buffer->cs, depth);
	} else {
		radeon_emit(cmd_buffer->cs, (region->extent.width -1) | ((region->extent.height - 1) << 16));
		radeon_emit(cmd_buffer->cs, (depth - 1));
	}
}

/* L2L buffer->image */
static void
radv_cik_dma_copy_one_image_lin_to_tiled(struct radv_cmd_buffer *cmd_buffer,
					 struct radv_image *lin_image,
					 const VkImageSubresourceLayers *lin_sub_resource,
					 const VkOffset3D *lin_offset,
					 struct radv_image *til_image,
					 const VkImageSubresourceLayers *til_sub_resource,
					 const VkOffset3D *til_offset,
					 const VkExtent3D *extent, bool lin2tiled)
{
	uint64_t lin_va, til_va;
	unsigned lin_pitch, lin_slice_pitch, lin_zoffset;
	unsigned til_pitch, til_slice_pitch, til_zoffset;
	unsigned bpp;
	unsigned lin_width = minify_as_blocks(lin_image->info.width,
					      lin_sub_resource->mipLevel, lin_image->surface.blk_w);
	unsigned til_width = minify_as_blocks(til_image->info.width,
					      til_sub_resource->mipLevel, til_image->surface.blk_w);
	radeon_check_space(cmd_buffer->device->ws, cmd_buffer->cs, 14);
	get_image_info(cmd_buffer, lin_image, lin_sub_resource, &lin_va,
		       &bpp, &lin_pitch, &lin_slice_pitch);
	get_image_info(cmd_buffer, til_image, til_sub_resource, &til_va,
		       NULL, &til_pitch, &til_slice_pitch);

	assert(til_pitch % 8 == 0);
	assert(til_slice_pitch % 64 == 0);
	unsigned pitch_tile_max = til_pitch / 8 - 1;
	unsigned slice_tile_max = til_slice_pitch / 64 - 1;
	unsigned xalign = MAX2(1, 4 / bpp);
	unsigned copy_width = DIV_ROUND_UP(extent->width, til_image->surface.blk_w);
	unsigned copy_height = DIV_ROUND_UP(extent->width, til_image->surface.blk_w);
	unsigned copy_width_aligned = copy_width;
	unsigned copy_depth;

	if (lin_image->type == VK_IMAGE_TYPE_3D) {
		copy_depth = extent->depth;
		lin_zoffset = lin_offset->z;
	} else {
		copy_depth = lin_sub_resource->layerCount;
		lin_zoffset = lin_sub_resource->baseArrayLayer;
	}

	if (til_image->type == VK_IMAGE_TYPE_3D) {
		til_zoffset = til_offset->z;
	} else {
		til_zoffset = til_sub_resource->baseArrayLayer;
	}

	/* If the region ends at the last pixel and is unaligned, we
	 * can copy the remainder of the line that is not visible to
	 * make it aligned.
	 */
	if (copy_width % xalign != 0 &&
	    lin_offset->x + copy_width == lin_width &&
	    til_offset->x  + copy_width == til_width &&
	    lin_offset->x + align(copy_width, xalign) <= lin_pitch &&
	    til_offset->x  + align(copy_width, xalign) <= til_pitch)
		copy_width_aligned = align(copy_width, xalign);

	/* TODO HW Limitations - how do we handle those in vk? */

	/* The hw can read outside of the given linear buffer bounds,
	 * or access those pages but not touch the memory in case
	 * of writes. (it still causes a VM fault)
	 *
	 * Out-of-bounds memory access or page directory access must
	 * be prevented.
	 */
	int64_t start_linear_address, end_linear_address;
	bool ret;
	unsigned granularity;
	ret = linear_buffer_workaround(cmd_buffer, til_image,
				       til_sub_resource->mipLevel,
				       bpp, &granularity);

	if (ret == false) {
		cmd_buffer->record_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
		return;
	}

	/* The linear reads start at tiled_x & ~(granularity - 1).
	 * If linear_x == 0 && tiled_x % granularity != 0, the hw
	 * starts reading from an address preceding linear_address!!!
	 */
	start_linear_address =
		lin_image->surface.u.legacy.level[lin_sub_resource->mipLevel].offset +
		bpp * (lin_offset->z * lin_slice_pitch +
		       lin_offset->y * lin_pitch +
		       lin_offset->x);
	start_linear_address -= (int)(bpp * (til_offset->x % granularity));

	end_linear_address =
		lin_image->surface.u.legacy.level[lin_sub_resource->mipLevel].offset +
		bpp * ((lin_offset->z + copy_depth - 1) * lin_slice_pitch +
		       (lin_offset->y + copy_height - 1) * lin_pitch +
		       (lin_offset->x + copy_width));

	if ((til_offset->x + copy_width) % granularity)
		end_linear_address += granularity -
			(til_offset->x + copy_width) % granularity;

	if (start_linear_address < 0 ||
	    end_linear_address > lin_image->surface.surf_size) {
		cmd_buffer->record_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
		return;
	}

	radeon_emit(cmd_buffer->cs, CIK_SDMA_PACKET(CIK_SDMA_OPCODE_COPY,
						    CIK_SDMA_COPY_SUB_OPCODE_TILED_SUB_WINDOW, 0) |
		    (lin2tiled ? 0 : (1u << 31)));
	radeon_emit(cmd_buffer->cs, til_va);
	radeon_emit(cmd_buffer->cs, til_va >> 32);
	radeon_emit(cmd_buffer->cs, til_offset->x | (til_offset->y << 16));
	radeon_emit(cmd_buffer->cs, til_zoffset | (pitch_tile_max << 16));
	radeon_emit(cmd_buffer->cs, slice_tile_max);
	radeon_emit(cmd_buffer->cs, encode_tile_info(cmd_buffer, til_image, til_sub_resource->mipLevel, true));
	radeon_emit(cmd_buffer->cs, lin_va);
	radeon_emit(cmd_buffer->cs, lin_va >> 32);
	radeon_emit(cmd_buffer->cs, lin_offset->x | (lin_offset->y << 16));
	radeon_emit(cmd_buffer->cs, lin_zoffset | ((lin_pitch - 1) << 16));
	radeon_emit(cmd_buffer->cs, lin_slice_pitch - 1);
	if (cmd_buffer->device->physical_device->rad_info.chip_class == CIK) {
		radeon_emit(cmd_buffer->cs, copy_width_aligned | (copy_height << 16));
		radeon_emit(cmd_buffer->cs, copy_depth);
	} else {
		radeon_emit(cmd_buffer->cs, (copy_width_aligned - 1) | ((copy_height - 1) << 16));
		radeon_emit(cmd_buffer->cs, (copy_depth - 1));
	}
}

static void
radv_cik_dma_copy_one_image_tiled_to_tiled(struct radv_cmd_buffer *cmd_buffer,
					   struct radv_image *src_image,
					   struct radv_image *dst_image,
					   const VkImageCopy *region)
{
	uint64_t src_va, dst_va;
	unsigned src_pitch, src_slice_pitch, src_zoffset;
	unsigned dst_pitch, dst_slice_pitch, dst_zoffset;
	unsigned depth;
	unsigned dst_width = minify_as_blocks(dst_image->info.width,
					      region->dstSubresource.mipLevel, dst_image->surface.blk_w);
	unsigned src_width = minify_as_blocks(src_image->info.width,
					      region->srcSubresource.mipLevel, src_image->surface.blk_w);
	unsigned dst_height = minify_as_blocks(dst_image->info.height,
					       region->dstSubresource.mipLevel, dst_image->surface.blk_h);
	unsigned src_height = minify_as_blocks(src_image->info.height,
					       region->srcSubresource.mipLevel, src_image->surface.blk_h);

	radeon_check_space(cmd_buffer->device->ws, cmd_buffer->cs, 15);
	get_image_info(cmd_buffer, src_image, &region->srcSubresource, &src_va,
		       NULL, &src_pitch, &src_slice_pitch);
	get_image_info(cmd_buffer, dst_image, &region->dstSubresource, &dst_va,
		       NULL, &dst_pitch, &dst_slice_pitch);

	unsigned src_pitch_tile_max = src_pitch / 8 - 1;
	unsigned src_slice_tile_max = src_slice_pitch / 64 - 1;

	unsigned dst_pitch_tile_max = dst_pitch / 8 - 1;
	unsigned dst_slice_tile_max = dst_slice_pitch / 64 - 1;

	unsigned copy_width = DIV_ROUND_UP(region->extent.width, src_image->surface.blk_w);
	unsigned copy_height = DIV_ROUND_UP(region->extent.height, src_image->surface.blk_h);

	unsigned copy_width_aligned = copy_width;
	unsigned copy_height_aligned = copy_height;

	if (copy_width % 8 != 0 &&
	    region->srcOffset.x + copy_width == src_width &&
	    region->dstOffset.x + copy_width == dst_width)
		copy_width_aligned = align(copy_width, 8);

	if (copy_height % 8 != 0 &&
	    region->srcOffset.y + copy_height == src_height &&
	    region->dstOffset.y + copy_height == dst_height)
		copy_height_aligned = align(copy_height, 8);

	if (src_image->type == VK_IMAGE_TYPE_3D) {
		depth = region->extent.depth;
		src_zoffset = region->srcOffset.z;
	} else {
		depth = region->srcSubresource.layerCount;
		src_zoffset = region->srcSubresource.baseArrayLayer;
	}

	if (dst_image->type == VK_IMAGE_TYPE_3D) {
		dst_zoffset = region->dstOffset.z;
	} else {
		dst_zoffset = region->dstSubresource.baseArrayLayer;
	}

	radeon_emit(cmd_buffer->cs, CIK_SDMA_PACKET(CIK_SDMA_OPCODE_COPY,
					CIK_SDMA_COPY_SUB_OPCODE_T2T_SUB_WINDOW, 0));
	radeon_emit(cmd_buffer->cs, src_va);
	radeon_emit(cmd_buffer->cs, src_va >> 32);
	radeon_emit(cmd_buffer->cs, region->srcOffset.x | (region->srcOffset.y << 16));
	radeon_emit(cmd_buffer->cs, src_zoffset | (src_pitch_tile_max << 16));
	radeon_emit(cmd_buffer->cs, src_slice_tile_max);
	radeon_emit(cmd_buffer->cs, encode_tile_info(cmd_buffer, src_image, region->srcSubresource.mipLevel, true));
	radeon_emit(cmd_buffer->cs, dst_va);
	radeon_emit(cmd_buffer->cs, dst_va >> 32);
	radeon_emit(cmd_buffer->cs, region->dstOffset.x | (region->dstOffset.y << 16));
	radeon_emit(cmd_buffer->cs, dst_zoffset | (dst_pitch_tile_max << 16));
	radeon_emit(cmd_buffer->cs, dst_slice_tile_max);
	radeon_emit(cmd_buffer->cs, encode_tile_info(cmd_buffer, dst_image, region->dstSubresource.mipLevel, false));
	if (cmd_buffer->device->physical_device->rad_info.chip_class == CIK) {
		radeon_emit(cmd_buffer->cs, copy_width_aligned | (copy_height_aligned << 16));
		radeon_emit(cmd_buffer->cs, depth);
	} else {
		radeon_emit(cmd_buffer->cs, (copy_width_aligned - 8) | ((copy_height_aligned - 8) << 16));
		radeon_emit(cmd_buffer->cs, (depth - 1));
	}
}

void radv_cik_dma_copy_image(struct radv_cmd_buffer *cmd_buffer,
			     struct radv_image *src_image,
			     VkImageLayout src_image_layout,
			     struct radv_image *dest_image,
			     VkImageLayout dest_image_layout,
			     uint32_t region_count,
			     const VkImageCopy *pRegions)
{
	uint32_t r;
	for (r = 0; r < region_count; r++) {
		const VkImageCopy *region = &pRegions[r];
		bool src_is_linear = src_image->surface.u.legacy.level[region->srcSubresource.mipLevel].mode == RADEON_SURF_MODE_LINEAR_ALIGNED;
		bool dst_is_linear = dest_image->surface.u.legacy.level[region->dstSubresource.mipLevel].mode == RADEON_SURF_MODE_LINEAR_ALIGNED;

		/* X -> X */
		if (src_is_linear && dst_is_linear) {
			radv_cik_dma_copy_one_image_lin_to_lin(cmd_buffer,
							       src_image,
							       dest_image,
							       region);
			/* L -> L */
		} else if (!src_is_linear && dst_is_linear) {
			/* T -> L */
			radv_cik_dma_copy_one_image_lin_to_tiled(cmd_buffer,
								 dest_image,
								 &region->dstSubresource,
								 &region->dstOffset,
								 src_image,
								 &region->srcSubresource,
								 &region->srcOffset,
								 &region->extent,
								 false);
		} else if (src_is_linear && !dst_is_linear) {
			/* L -> T */
			radv_cik_dma_copy_one_image_lin_to_tiled(cmd_buffer,
								 src_image,
								 &region->srcSubresource,
								 &region->srcOffset,
								 dest_image,
								 &region->dstSubresource,
								 &region->dstOffset,
								 &region->extent,
								 true);
		} else {
			/* T -> T */
			radv_cik_dma_copy_one_image_tiled_to_tiled(cmd_buffer,
								   src_image,
								   dest_image,
								   region);
		}
	}

}

static void
radv_cik_sdma_do_copy_buffer_one(struct radv_cmd_buffer *cmd_buffer,
				 struct radv_buffer *src_buffer,
				 struct radv_buffer *dst_buffer,
				 const VkBufferCopy *region)
{
	unsigned ncopy, i;
	uint64_t src_va, dst_va;
	VkDeviceSize size = region->size;

	src_va = radv_buffer_get_va(src_buffer->bo);
	dst_va = radv_buffer_get_va(dst_buffer->bo);

	src_va += src_buffer->offset;
	dst_va += dst_buffer->offset;
	ncopy = DIV_ROUND_UP(region->size, CIK_SDMA_COPY_MAX_SIZE);

	src_va += region->srcOffset;
	dst_va += region->dstOffset;

	radeon_check_space(cmd_buffer->device->ws, cmd_buffer->cs, ncopy * 7);
	for (i = 0; i < ncopy; i++) {
		unsigned csize = MIN2(size, CIK_SDMA_COPY_MAX_SIZE);

		radeon_emit(cmd_buffer->cs, CIK_SDMA_PACKET(CIK_SDMA_OPCODE_COPY,
							    CIK_SDMA_COPY_SUB_OPCODE_LINEAR,
							    0));
		if (cmd_buffer->device->physical_device->rad_info.chip_class >= GFX9)
			radeon_emit(cmd_buffer->cs, csize - 1);
		else
			radeon_emit(cmd_buffer->cs, csize);
		radeon_emit(cmd_buffer->cs, 0);
		radeon_emit(cmd_buffer->cs, src_va);
		radeon_emit(cmd_buffer->cs, src_va >> 32);
		radeon_emit(cmd_buffer->cs, dst_va);
		radeon_emit(cmd_buffer->cs, dst_va >> 32);
		dst_va += csize;
		src_va += csize;
		size -= csize;
	}
}

void radv_cik_dma_copy_buffer(struct radv_cmd_buffer *cmd_buffer,
			      struct radv_buffer *src_buffer,
			      struct radv_buffer *dest_buffer,
			      uint32_t region_count,
			      const VkBufferCopy *pRegions)
{
	int r;

	for (r = 0; r < region_count; r++)
		radv_cik_sdma_do_copy_buffer_one(cmd_buffer,
						 src_buffer,
						 dest_buffer,
						 &pRegions[r]);
}

void radv_cik_dma_update_buffer(struct radv_cmd_buffer *cmd_buffer,
				struct radv_buffer *dst_buffer,
				VkDeviceSize dst_offset,
				VkDeviceSize data_size,
				const void *data)
{
	uint64_t dst_va = radv_buffer_get_va(dst_buffer->bo);
	int num_dw = (data_size + 3) / 4;
	dst_va += dst_buffer->offset;
	dst_va += dst_offset;

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
		radeon_emit(cmd_buffer->cs, this_dw);
		radeon_emit_array(cmd_buffer->cs, data_dw, this_dw);

		data_dw += this_dw;
		left_dw -= this_dw;

		if (left_dw)
			radeon_check_space(cmd_buffer->device->ws, cmd_buffer->cs, left_dw + 4);
	} while (left_dw > 0);
}

void radv_cik_dma_fill_buffer(struct radv_cmd_buffer *cmd_buffer,
			      struct radv_buffer *dst_buffer,
			      VkDeviceSize dst_offset,
			      VkDeviceSize fillSize,
			      uint32_t data)
{
	uint64_t dst_va = radv_buffer_get_va(dst_buffer->bo);
	uint32_t size = fillSize;

	if (cmd_buffer->device->physical_device->rad_info.chip_class >= GFX9)
		size--;
	dst_va += dst_buffer->offset;
	dst_va += dst_offset;
	radeon_emit(cmd_buffer->cs, CIK_SDMA_PACKET(CIK_SDMA_PACKET_CONSTANT_FILL,
						    0, SDMA_CONSTANT_FILL_DWORDS));
	radeon_emit(cmd_buffer->cs, dst_va);
	radeon_emit(cmd_buffer->cs, dst_va >> 32);
	radeon_emit(cmd_buffer->cs, data);
	radeon_emit(cmd_buffer->cs, size);
}
