/*
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
#include "vk_format.h"

#include "sid.h"
#include "radv_cs.h"

static VkExtent3D
meta_image_block_size(const struct radv_image *image)
{
	const struct vk_format_description *desc = vk_format_description(image->vk_format);
	return (VkExtent3D) { desc->block.width, desc->block.height, 1 };
}

/* Returns the user-provided VkBufferImageCopy::imageExtent in units of
 * elements rather than texels. One element equals one texel or one block
 * if Image is uncompressed or compressed, respectively.
 */
static struct VkExtent3D
meta_region_extent_el(const struct radv_image *image,
                      const struct VkExtent3D *extent)
{
	const VkExtent3D block = meta_image_block_size(image);
	return radv_sanitize_image_extent(image->type, (VkExtent3D) {
			.width  = DIV_ROUND_UP(extent->width , block.width),
				.height = DIV_ROUND_UP(extent->height, block.height),
				.depth  = DIV_ROUND_UP(extent->depth , block.depth),
				});
}

/* Returns the user-provided VkBufferImageCopy::imageOffset in units of
 * elements rather than texels. One element equals one texel or one block
 * if Image is uncompressed or compressed, respectively.
 */
static struct VkOffset3D
meta_region_offset_el(const struct radv_image *image,
                      const struct VkOffset3D *offset)
{
	const VkExtent3D block = meta_image_block_size(image);
	return radv_sanitize_image_offset(image->type, (VkOffset3D) {
			.x = offset->x / block.width,
				.y = offset->y / block.height,
				.z = offset->z / block.depth,
				});
}

static struct radv_meta_blit2d_surf
blit_surf_for_image(const struct radv_image* image,
                    const struct radeon_surf *surf)
{
	int tiling = RADEON_SURF_GET(surf->flags, MODE) == RADEON_SURF_MODE_LINEAR_ALIGNED ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL;
	return (struct radv_meta_blit2d_surf) {
		.bo = image->bo,
			.base_offset = image->offset,
			.bs = vk_format_get_blocksize(image->vk_format),
			.pitch = surf->level[0].pitch_bytes,
			.tiling = tiling,
			};
}

static struct radv_meta_blit2d_surf
blit_surf_for_image_level_layer(const struct radv_image* image,
				const struct radeon_surf *surf, int level, int layer)
{
	int tiling = RADEON_SURF_GET(surf->flags, MODE) == RADEON_SURF_MODE_LINEAR_ALIGNED ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL;
	return (struct radv_meta_blit2d_surf) {
		.bo = image->bo,
			.base_offset = image->offset + surf->level[level].offset + (layer * surf->level[level].slice_size),
			.bs = vk_format_get_blocksize(image->vk_format),
			.pitch = surf->level[level].pitch_bytes,
			.tiling = tiling,
			.slice_size = surf->level[level].slice_size,
			};
}

/* Set this if you want the 3D engine to wait until CP DMA is done.
 * It should be set on the last CP DMA packet. */
#define R600_CP_DMA_SYNC	(1 << 0) /* R600+ */

/* Set this if the source data was used as a destination in a previous CP DMA
 * packet. It's for preventing a read-after-write (RAW) hazard between two
 * CP DMA packets. */
#define SI_CP_DMA_RAW_WAIT	(1 << 1) /* SI+ */
#define CIK_CP_DMA_USE_L2	(1 << 2)

/* Alignment for optimal performance. */
#define CP_DMA_ALIGNMENT	32
/* The max number of bytes to copy per packet. */
#define CP_DMA_MAX_BYTE_COUNT	((1 << 21) - CP_DMA_ALIGNMENT)

static void si_emit_cp_dma_copy_buffer(struct radv_cmd_buffer *cmd_buffer,
				       uint64_t dst_va, uint64_t src_va,
				       unsigned size, unsigned flags)
{
	struct radeon_winsys_cs *cs = cmd_buffer->cs;
	uint32_t sync_flag = flags & R600_CP_DMA_SYNC ? S_411_CP_SYNC(1) : 0;
	uint32_t wr_confirm = !(flags & R600_CP_DMA_SYNC) ? S_414_DISABLE_WR_CONFIRM(1) : 0;
	uint32_t raw_wait = flags & SI_CP_DMA_RAW_WAIT ? S_414_RAW_WAIT(1) : 0;
	uint32_t sel = flags & CIK_CP_DMA_USE_L2 ?
			   S_411_SRC_SEL(V_411_SRC_ADDR_TC_L2) |
			   S_411_DSL_SEL(V_411_DST_ADDR_TC_L2) : 0;

	assert(size);
	assert((size & ((1<<21)-1)) == size);

	radeon_check_space(cmd_buffer->device->ws, cmd_buffer->cs, 9);

	if (cmd_buffer->device->instance->physicalDevice.rad_info.chip_class >= CIK) {
		radeon_emit(cs, PKT3(PKT3_DMA_DATA, 5, 0));
		radeon_emit(cs, sync_flag | sel);	/* CP_SYNC [31] */
		radeon_emit(cs, src_va);		/* SRC_ADDR_LO [31:0] */
		radeon_emit(cs, src_va >> 32);		/* SRC_ADDR_HI [31:0] */
		radeon_emit(cs, dst_va);		/* DST_ADDR_LO [31:0] */
		radeon_emit(cs, dst_va >> 32);		/* DST_ADDR_HI [31:0] */
		radeon_emit(cs, size | wr_confirm | raw_wait);	/* COMMAND [29:22] | BYTE_COUNT [20:0] */
	} else {
		radeon_emit(cs, PKT3(PKT3_CP_DMA, 4, 0));
		radeon_emit(cs, src_va);			/* SRC_ADDR_LO [31:0] */
		radeon_emit(cs, sync_flag | ((src_va >> 32) & 0xffff)); /* CP_SYNC [31] | SRC_ADDR_HI [15:0] */
		radeon_emit(cs, dst_va);			/* DST_ADDR_LO [31:0] */
		radeon_emit(cs, (dst_va >> 32) & 0xffff);	/* DST_ADDR_HI [15:0] */
		radeon_emit(cs, size | wr_confirm | raw_wait);	/* COMMAND [29:22] | BYTE_COUNT [20:0] */
	}

	/* CP DMA is executed in ME, but index buffers are read by PFP.
	 * This ensures that ME (CP DMA) is idle before PFP starts fetching
	 * indices. If we wanted to execute CP DMA in PFP, this packet
	 * should precede it.
	 */
	if (sync_flag) {
		radeon_emit(cs, PKT3(PKT3_PFP_SYNC_ME, 0, 0));
		radeon_emit(cs, 0);
	}
}

static void si_cp_dma_prepare(struct radv_cmd_buffer *cmd_buffer, uint64_t byte_count,
			      uint64_t remaining_size, unsigned *flags)
{

	/* Flush the caches for the first copy only.
	 * Also wait for the previous CP DMA operations.
	 */
	if (cmd_buffer->state.flush_bits) {
		si_emit_cache_flush(cmd_buffer);
		*flags |= SI_CP_DMA_RAW_WAIT;
	}

	/* Do the synchronization after the last dma, so that all data
	 * is written to memory.
	 */
	if (byte_count == remaining_size)
		*flags |= R600_CP_DMA_SYNC;
}

static void si_cp_dma_realign_engine(struct radv_cmd_buffer *cmd_buffer, unsigned size)
{
	uint64_t va;
	uint32_t offset;
	unsigned dma_flags = 0;
	unsigned buf_size = CP_DMA_ALIGNMENT * 2;
	void *ptr;

	assert(size < CP_DMA_ALIGNMENT);

	radv_cmd_buffer_upload_alloc(cmd_buffer, buf_size, CP_DMA_ALIGNMENT,  &offset, &ptr);

	va = cmd_buffer->device->ws->buffer_get_va(cmd_buffer->upload.upload_bo.bo);
	va += offset;

	si_cp_dma_prepare(cmd_buffer, size, size, &dma_flags);

	si_emit_cp_dma_copy_buffer(cmd_buffer, va, va + CP_DMA_ALIGNMENT, size,
				   dma_flags);
}

static void
do_buffer_copy(struct radv_cmd_buffer *cmd_buffer,
               struct radv_bo *src, uint64_t src_offset,
               struct radv_bo *dest, uint64_t dest_offset,
	       uint64_t size)
{
	struct radv_device *device = cmd_buffer->device;
	struct radeon_winsys_cs *cs = cmd_buffer->cs;
	uint64_t src_va, dest_va, main_src_va, main_dest_va;
	uint64_t skipped_size = 0, realign_size = 0;

	device->ws->cs_add_buffer(cs, src->bo, 8);
	device->ws->cs_add_buffer(cs, dest->bo, 8);

	src_va = device->ws->buffer_get_va(src->bo) + src_offset;
	dest_va = device->ws->buffer_get_va(dest->bo) + dest_offset;

	if (cmd_buffer->device->instance->physicalDevice.rad_info.family <= CHIP_CARRIZO ||
	    cmd_buffer->device->instance->physicalDevice.rad_info.family == CHIP_STONEY) {
		/* If the size is not aligned, we must add a dummy copy at the end
		 * just to align the internal counter. Otherwise, the DMA engine
		 * would slow down by an order of magnitude for following copies.
		 */
		if (size % CP_DMA_ALIGNMENT)
			realign_size = CP_DMA_ALIGNMENT - (size % CP_DMA_ALIGNMENT);

		/* If the copy begins unaligned, we must start copying from the next
		 * aligned block and the skipped part should be copied after everything
		 * else has been copied. Only the src alignment matters, not dst.
		 */
		if (src_offset % CP_DMA_ALIGNMENT) {
			skipped_size = CP_DMA_ALIGNMENT - (src_offset % CP_DMA_ALIGNMENT);
			/* The main part will be skipped if the size is too small. */
			skipped_size = MIN2(skipped_size, size);
			size -= skipped_size;
		}
	}
	main_src_va = src_va + skipped_size;
	main_dest_va = dest_va + skipped_size;

	while (size) {
		unsigned dma_flags = 0;
		unsigned byte_count = MIN2(size, CP_DMA_MAX_BYTE_COUNT);

		si_cp_dma_prepare(cmd_buffer, byte_count,
				  size + skipped_size + realign_size,
				  &dma_flags);

		si_emit_cp_dma_copy_buffer(cmd_buffer, main_dest_va, main_src_va,
					   byte_count, dma_flags);

		size -= byte_count;
		main_src_va += byte_count;
		main_dest_va += byte_count;
	}

	if (skipped_size) {
		unsigned dma_flags = 0;

		si_cp_dma_prepare(cmd_buffer, skipped_size,
				  size + skipped_size + realign_size,
				  &dma_flags);

		si_emit_cp_dma_copy_buffer(cmd_buffer, dest_va, src_va,
					   skipped_size, dma_flags);
	}
	if (realign_size)
		si_cp_dma_realign_engine(cmd_buffer, realign_size);
}

static void
meta_copy_buffer_to_image(struct radv_cmd_buffer *cmd_buffer,
                          struct radv_buffer* buffer,
                          struct radv_image* image,
                          uint32_t regionCount,
                          const VkBufferImageCopy* pRegions)
{
	struct radv_meta_saved_state saved_state;

	/* The Vulkan 1.0 spec says "dstImage must have a sample count equal to
	 * VK_SAMPLE_COUNT_1_BIT."
	 */
	assert(image->samples == 1);

	radv_meta_begin_blit2d(cmd_buffer, &saved_state);

	for (unsigned r = 0; r < regionCount; r++) {

		/**
		 * From the Vulkan 1.0.6 spec: 18.3 Copying Data Between Images
		 *    extent is the size in texels of the source image to copy in width,
		 *    height and depth. 1D images use only x and width. 2D images use x, y,
		 *    width and height. 3D images use x, y, z, width, height and depth.
		 *
		 *
		 * Also, convert the offsets and extent from units of texels to units of
		 * blocks - which is the highest resolution accessible in this command.
		 */
		const VkOffset3D img_offset_el =
			meta_region_offset_el(image, &pRegions[r].imageOffset);
		const VkExtent3D bufferExtent = {
			.width  = pRegions[r].bufferRowLength ?
			pRegions[r].bufferRowLength : pRegions[r].imageExtent.width,
			.height = pRegions[r].bufferImageHeight ?
			pRegions[r].bufferImageHeight : pRegions[r].imageExtent.height,
		};
		const VkExtent3D buf_extent_el =
			meta_region_extent_el(image, &bufferExtent);

		/* Start creating blit rect */
		const VkExtent3D img_extent_el =
			meta_region_extent_el(image, &pRegions[r].imageExtent);
		struct radv_meta_blit2d_rect rect = {
			.width = img_extent_el.width,
			.height =  img_extent_el.height,
		};

		/* Create blit surfaces */
		VkImageAspectFlags aspect = pRegions[r].imageSubresource.aspectMask;
		const struct radeon_surf *img_surf = &image->surface;
		struct radv_meta_blit2d_surf img_bsurf =
			blit_surf_for_image_level_layer(image, img_surf, pRegions[r].imageSubresource.mipLevel,
							pRegions[r].imageSubresource.baseArrayLayer);
		struct radv_meta_blit2d_buffer buf_bsurf = {
			.bs = img_bsurf.bs,
			.buffer = buffer,
			.offset = pRegions[r].bufferOffset,
			.pitch = buf_extent_el.width,
		};

		/* Loop through each 3D or array slice */
		unsigned num_slices_3d = img_extent_el.depth;
		unsigned num_slices_array = pRegions[r].imageSubresource.layerCount;
		unsigned slice_3d = 0;
		unsigned slice_array = 0;
		while (slice_3d < num_slices_3d && slice_array < num_slices_array) {

			rect.dst_x += img_offset_el.x;
			rect.dst_y += img_offset_el.y;

			/* Perform Blit */
			radv_meta_blit2d(cmd_buffer, NULL, &buf_bsurf, &img_bsurf, 1, &rect);

			/* Once we've done the blit, all of the actual information about
			 * the image is embedded in the command buffer so we can just
			 * increment the offset directly in the image effectively
			 * re-binding it to different backing memory.
			 */
			buf_bsurf.offset += buf_extent_el.width *
			                    buf_extent_el.height * buf_bsurf.bs;
			img_bsurf.base_offset += img_bsurf.slice_size;
			if (image->type == VK_IMAGE_TYPE_3D)
				slice_3d++;
			else
				slice_array++;
		}
	}
	radv_meta_end_blit2d(cmd_buffer, &saved_state);
}

void radv_CmdCopyBufferToImage(
	VkCommandBuffer                             commandBuffer,
	VkBuffer                                    srcBuffer,
	VkImage                                     destImage,
	VkImageLayout                               destImageLayout,
	uint32_t                                    regionCount,
	const VkBufferImageCopy*                    pRegions)
{
	RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
	RADV_FROM_HANDLE(radv_image, dest_image, destImage);
	RADV_FROM_HANDLE(radv_buffer, src_buffer, srcBuffer);

	meta_copy_buffer_to_image(cmd_buffer, src_buffer, dest_image,
				  regionCount, pRegions);
}

static void
meta_copy_image_to_buffer(struct radv_cmd_buffer *cmd_buffer,
                          struct radv_buffer* buffer,
                          struct radv_image* image,
                          uint32_t regionCount,
                          const VkBufferImageCopy* pRegions)
{
	struct radv_meta_saved_state saved_state;

	radv_meta_begin_bufimage(cmd_buffer, &saved_state);
	for (unsigned r = 0; r < regionCount; r++) {
		const VkExtent3D img_extent_el =
			meta_region_extent_el(image, &pRegions[r].imageExtent);
		struct radv_meta_blit2d_rect rect = {
			.width = img_extent_el.width,
			.height =  img_extent_el.height,
		};
		const struct radeon_surf *img_surf = &image->surface;
		struct radv_meta_blit2d_surf img_bsurf =
			blit_surf_for_image_level_layer(image, img_surf, pRegions[r].imageSubresource.mipLevel, pRegions[r].imageSubresource.baseArrayLayer);

		radv_meta_image_to_buffer(cmd_buffer, &img_bsurf,
					  buffer, 1, &rect);
	}
}

void radv_CmdCopyImageToBuffer(
	VkCommandBuffer                             commandBuffer,
	VkImage                                     srcImage,
	VkImageLayout                               srcImageLayout,
	VkBuffer                                    destBuffer,
	uint32_t                                    regionCount,
	const VkBufferImageCopy*                    pRegions)
{
	RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
	RADV_FROM_HANDLE(radv_image, src_image, srcImage);
	RADV_FROM_HANDLE(radv_buffer, dst_buffer, destBuffer);

	meta_copy_image_to_buffer(cmd_buffer, dst_buffer, src_image,
				  regionCount, pRegions);
}

void radv_CmdCopyImage(
	VkCommandBuffer                             commandBuffer,
	VkImage                                     srcImage,
	VkImageLayout                               srcImageLayout,
	VkImage                                     destImage,
	VkImageLayout                               destImageLayout,
	uint32_t                                    regionCount,
	const VkImageCopy*                          pRegions)
{
	RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
	RADV_FROM_HANDLE(radv_image, src_image, srcImage);
	RADV_FROM_HANDLE(radv_image, dest_image, destImage);
	struct radv_meta_saved_state saved_state;

	/* From the Vulkan 1.0 spec:
	 *
	 *    vkCmdCopyImage can be used to copy image data between multisample
	 *    images, but both images must have the same number of samples.
	 */
	assert(src_image->samples == dest_image->samples);

	radv_meta_begin_blit2d(cmd_buffer, &saved_state);

	for (unsigned r = 0; r < regionCount; r++) {
		assert(pRegions[r].srcSubresource.aspectMask ==
		       pRegions[r].dstSubresource.aspectMask);

		VkImageAspectFlags aspect = pRegions[r].srcSubresource.aspectMask;

		/* Create blit surfaces */
		struct radeon_surf *src_surf = &src_image->surface;
		struct radeon_surf *dst_surf = &dest_image->surface;
		struct radv_meta_blit2d_surf b_src =
			blit_surf_for_image(src_image, src_surf);
		struct radv_meta_blit2d_surf b_dst =
			blit_surf_for_image(dest_image, dst_surf);

		/**
		 * From the Vulkan 1.0.6 spec: 18.4 Copying Data Between Buffers and Images
		 *    imageExtent is the size in texels of the image to copy in width, height
		 *    and depth. 1D images use only x and width. 2D images use x, y, width
		 *    and height. 3D images use x, y, z, width, height and depth.
		 *
		 * Also, convert the offsets and extent from units of texels to units of
		 * blocks - which is the highest resolution accessible in this command.
		 */
		const VkOffset3D dst_offset_el =
			meta_region_offset_el(dest_image, &pRegions[r].dstOffset);
		const VkOffset3D src_offset_el =
			meta_region_offset_el(src_image, &pRegions[r].srcOffset);
		const VkExtent3D img_extent_el =
			meta_region_extent_el(src_image, &pRegions[r].extent);

		/* Start creating blit rect */
		struct radv_meta_blit2d_rect rect = {
			.width = img_extent_el.width,
			.height = img_extent_el.height,
		};

		/* Loop through each 3D or array slice */
		unsigned num_slices_3d = img_extent_el.depth;
		unsigned num_slices_array = pRegions[r].dstSubresource.layerCount;
		unsigned slice_3d = 0;
		unsigned slice_array = 0;
		while (slice_3d < num_slices_3d && slice_array < num_slices_array) {

			/* Finish creating blit rect */
#if 0
			isl_surf_get_image_offset_el(&dst_surf->isl,
						     pRegions[r].dstSubresource.mipLevel,
						     pRegions[r].dstSubresource.baseArrayLayer
						     + slice_array,
						     dst_offset_el.z + slice_3d,
						     &rect.dst_x,
						     &rect.dst_y);
			isl_surf_get_image_offset_el(&src_surf->isl,
						     pRegions[r].srcSubresource.mipLevel,
						     pRegions[r].srcSubresource.baseArrayLayer
						     + slice_array,
						     src_offset_el.z + slice_3d,
						     &rect.src_x,
						     &rect.src_y);
#endif
			rect.dst_x += dst_offset_el.x;
			rect.dst_y += dst_offset_el.y;
			rect.src_x += src_offset_el.x;
			rect.src_y += src_offset_el.y;

			/* Perform Blit */
			radv_meta_blit2d(cmd_buffer, &b_src, NULL, &b_dst, 1, &rect);

			if (dest_image->type == VK_IMAGE_TYPE_3D)
				slice_3d++;
			else
				slice_array++;
		}
	}

	radv_meta_end_blit2d(cmd_buffer, &saved_state);
}

void radv_CmdCopyBuffer(
	VkCommandBuffer                             commandBuffer,
	VkBuffer                                    srcBuffer,
	VkBuffer                                    destBuffer,
	uint32_t                                    regionCount,
	const VkBufferCopy*                         pRegions)
{
	RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
	RADV_FROM_HANDLE(radv_buffer, src_buffer, srcBuffer);
	RADV_FROM_HANDLE(radv_buffer, dest_buffer, destBuffer);

	struct radv_meta_saved_state saved_state;

	for (unsigned r = 0; r < regionCount; r++) {
		uint64_t src_offset = src_buffer->offset + pRegions[r].srcOffset;
		uint64_t dest_offset = dest_buffer->offset + pRegions[r].dstOffset;
		uint64_t copy_size = pRegions[r].size;

		do_buffer_copy(cmd_buffer, src_buffer->bo, src_offset,
				       dest_buffer->bo, dest_offset, copy_size);
	}
}

void radv_CmdUpdateBuffer(
	VkCommandBuffer                             commandBuffer,
	VkBuffer                                    dstBuffer,
	VkDeviceSize                                dstOffset,
	VkDeviceSize                                dataSize,
	const uint32_t*                             pData)
{
	RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
	RADV_FROM_HANDLE(radv_buffer, dst_buffer, dstBuffer);
	struct radv_meta_saved_state saved_state;

#if 0
	do_buffer_copy(cmd_buffer, src_buffer->bo, src_offset,
		       dst_buffer->bo, dstOffset, dataSize);
#endif
}
