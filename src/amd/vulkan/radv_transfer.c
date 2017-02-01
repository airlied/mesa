/*
 * Copyright © 2018 Red Hat.
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
#include "vk_format.h"

static VkFormat get_format_from_aspect_mask(VkImageAspectFlags aspectMask,
					    VkFormat format)
{
	if (aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT)
		format = vk_format_depth_only(format);
	else if (aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT)
		format = vk_format_stencil_only(format);
	return format;
}

static
void radv_transfer_get_per_image_info(struct radv_device *device,
				      struct radv_image *image,
				      const VkImageSubresourceLayers *subres,
				      const VkOffset3D *offset,
				      struct radv_transfer_per_image_info *info)
{
	VkFormat format = get_format_from_aspect_mask(subres->aspectMask, image->vk_format);

	info->bpp = vk_format_get_blocksize(format);
	info->mip_level = subres->mipLevel;

	info->offset = *offset;
	if (image->type != VK_IMAGE_TYPE_3D) {
		info->offset.z = subres->baseArrayLayer;
	}
	device->transfer_fns->get_per_image_info(image, subres->aspectMask == VK_IMAGE_ASPECT_STENCIL_BIT, info);
}

static
void radv_transfer_get_image_info(struct radv_device *device,
				  struct radv_image *src_image,
				  struct radv_image *dst_image,
				  const VkImageCopy *region,
				  struct radv_transfer_image_info *info)
{
	radv_transfer_get_per_image_info(device, src_image, &region->srcSubresource, &region->srcOffset,
					 &info->src_info);
	radv_transfer_get_per_image_info(device, dst_image, &region->dstSubresource, &region->dstOffset,
					 &info->dst_info);

	info->extent = region->extent;
	if (src_image->type != VK_IMAGE_TYPE_3D)
		info->extent.depth = region->srcSubresource.layerCount;
}

static void
get_buffer_info(const VkBufferImageCopy *region,
		struct radv_buffer *buf,
		uint32_t block_width, uint32_t block_height,
		struct radv_transfer_per_buffer_info *info)
{
	uint32_t rowLength = region->bufferRowLength;
	uint32_t imageHeight = region->bufferImageHeight;

	if (rowLength == 0)
		rowLength = region->imageExtent.width;

	if (imageHeight == 0)
		imageHeight = region->imageExtent.height;

	info->va = radv_buffer_get_va(buf->bo) + buf->offset + region->bufferOffset;
	info->pitch = rowLength / block_width;
	info->slice_pitch = info->pitch * imageHeight / block_height;
}

static
void radv_transfer_get_buffer_image_info(struct radv_device *device,
					 struct radv_buffer *buf,
					 struct radv_image *image,
					 const VkBufferImageCopy *region,
					 struct radv_transfer_image_buffer_info *info)
{
	radv_transfer_get_per_image_info(device, image, &region->imageSubresource, &region->imageOffset,
					 &info->image_info);
	get_buffer_info(region, buf, image->planes[0].surface.blk_w, image->planes[0].surface.blk_h, &info->buf_info);

	info->extent = region->imageExtent;
	if (image->type != VK_IMAGE_TYPE_3D)
		info->extent.depth = region->imageSubresource.layerCount;
}

void radv_transfer_cmd_copy_buffer_to_image(struct radv_cmd_buffer *cmd_buffer,
					    struct radv_buffer *src_buffer,
					    struct radv_image *dst_image,
					    VkImageLayout dst_image_layout,
					    uint32_t region_count,
					    const VkBufferImageCopy *pregions)
{
	const struct radv_transfer_fns *xfer_fns = cmd_buffer->device->transfer_fns;
	for (uint32_t r = 0; r < region_count; r++) {
		struct radv_transfer_image_buffer_info info;

		radv_transfer_get_buffer_image_info(cmd_buffer->device,
						    src_buffer, dst_image,
						    &pregions[r], &info);
		if (dst_image->planes[0].surface.is_linear)
			xfer_fns->copy_buffer_image_l2l(cmd_buffer,
							&info,
							true);
		else
			xfer_fns->copy_buffer_image_l2t(cmd_buffer,
							&info,
							dst_image,
							true);
	}
}

void radv_transfer_cmd_copy_image_to_buffer(struct radv_cmd_buffer *cmd_buffer,
					    struct radv_image *src_image,
					    VkImageLayout src_image_layout,
					    struct radv_buffer *dst_buffer,
					    uint32_t region_count,
					    const VkBufferImageCopy *pregions)
{
	const struct radv_transfer_fns *xfer_fns = cmd_buffer->device->transfer_fns;
	for (uint32_t r = 0; r < region_count; r++) {
		struct radv_transfer_image_buffer_info info;
		radv_transfer_get_buffer_image_info(cmd_buffer->device,
						    dst_buffer, src_image,
						    &pregions[r], &info);
		if (src_image->planes[0].surface.is_linear)
			xfer_fns->copy_buffer_image_l2l(cmd_buffer,
							&info,
							false);
		else
			xfer_fns->copy_buffer_image_l2t(cmd_buffer,
							&info,
							src_image,
							false);
	}
}

static void
radv_transfer_alloc_temp_buffer(struct radv_cmd_buffer *cmd_buffer,
				struct radv_buffer *temp_buf)
{
#define TEMP_SIZE (128 * 1024 * 4)
	/* amdvlk allocate 128k dwords */
	if (!cmd_buffer->transfer_temp_bo) {
		cmd_buffer->transfer_temp_bo = cmd_buffer->device->ws->buffer_create(cmd_buffer->device->ws,
										     TEMP_SIZE,
										     4096,
										     RADEON_DOMAIN_VRAM,
										     RADEON_FLAG_NO_CPU_ACCESS | RADEON_FLAG_NO_INTERPROCESS_SHARING | RADEON_FLAG_32BIT, RADV_BO_PRIORITY_CS);
		radv_cs_add_buffer(cmd_buffer->device->ws, cmd_buffer->cs, cmd_buffer->transfer_temp_bo);
	}
	temp_buf->bo = cmd_buffer->transfer_temp_bo;
	temp_buf->size = TEMP_SIZE;
	temp_buf->offset = 0;
}
#undef TEMP_SIZE

static void
radv_transfer_cmd_copy_image_t2t_scanline(struct radv_cmd_buffer *cmd_buffer,
					  const struct radv_transfer_image_info *info,
					  struct radv_image *src_image,
					  struct radv_image *dst_image)
{
	const struct radv_transfer_fns *xfer_fns = cmd_buffer->device->transfer_fns;
	struct radv_transfer_image_buffer_info src_to_temp_info;
	struct radv_transfer_image_buffer_info temp_to_dst_info;
	struct radv_buffer temp_buf = {};

	radv_transfer_alloc_temp_buffer(cmd_buffer, &temp_buf);

	uint32_t copy_size_dwords = MIN2(temp_buf.size / sizeof(uint32_t), (info->extent.width * info->src_info.bpp) / sizeof(uint32_t));
	uint32_t copy_size_bytes = copy_size_dwords * sizeof(uint32_t);
	uint32_t copy_size_pixels = copy_size_bytes / info->src_info.bpp;
	VkBufferImageCopy region = {};
	region.imageExtent.width = copy_size_pixels;
	region.imageExtent.height = 1;
	region.imageExtent.depth = 1;

	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.layerCount = 1;

	region.bufferOffset = 0;
	region.bufferRowLength = copy_size_pixels;
	region.bufferImageHeight = 1;

	for (uint32_t slice = 0; slice < info->extent.depth; slice++) {
		for (uint32_t y = 0; y < info->extent.height; y++) {
			for (uint32_t x = 0; x < info->extent.width; x += copy_size_pixels) {
				VkBufferImageCopy src_region = region;
				VkBufferImageCopy dst_region = region;

				src_region.imageOffset.x = x + info->src_info.offset.x;
				src_region.imageOffset.y = y + info->src_info.offset.y;

				src_region.imageSubresource.mipLevel = info->src_info.mip_level;

				if (src_image->type != VK_IMAGE_TYPE_3D)
					src_region.imageSubresource.baseArrayLayer = slice + info->src_info.offset.z;
				else
					src_region.imageOffset.z = slice + info->src_info.offset.z;

				dst_region.imageOffset.x = x + info->dst_info.offset.x;
				dst_region.imageOffset.y = y + info->dst_info.offset.y;

				dst_region.imageSubresource.mipLevel = info->dst_info.mip_level;

				if (dst_image->type != VK_IMAGE_TYPE_3D)
					dst_region.imageSubresource.baseArrayLayer = slice + info->dst_info.offset.z;
				else
					dst_region.imageOffset.z = slice + info->dst_info.offset.z;

			       radv_transfer_get_buffer_image_info(cmd_buffer->device,
								    &temp_buf,
								    src_image,
								    &src_region,
								    &src_to_temp_info);

				radv_transfer_get_buffer_image_info(cmd_buffer->device,
								    &temp_buf,
								    dst_image,
								    &dst_region,
								    &temp_to_dst_info);

				xfer_fns->copy_buffer_image_l2t(cmd_buffer,
								&src_to_temp_info,
								src_image,
								false);
				xfer_fns->emit_nop(cmd_buffer);
				xfer_fns->copy_buffer_image_l2t(cmd_buffer,
								&temp_to_dst_info,
								dst_image,
								true);
				xfer_fns->emit_nop(cmd_buffer);
			}
		}
	}
}

void radv_transfer_cmd_copy_image(struct radv_cmd_buffer *cmd_buffer,
				  struct radv_image *src_image,
				  VkImageLayout src_image_layout,
				  struct radv_image *dst_image,
				  VkImageLayout dst_image_layout,
				  uint32_t region_count,
				  const VkImageCopy *pregions)
{
	const struct radv_transfer_fns *xfer_fns = cmd_buffer->device->transfer_fns;
	for (uint32_t r = 0; r < region_count; r++) {
		const VkImageCopy *region = &pregions[r];
		struct radv_transfer_image_info info;

		radv_transfer_get_image_info(cmd_buffer->device,
					     src_image,
					     dst_image,
					     region,
					     &info);

		if (src_image->planes[0].surface.is_linear && dst_image->planes[0].surface.is_linear)
			xfer_fns->copy_image_l2l(cmd_buffer, &info, src_image, dst_image);
		else if (src_image->planes[0].surface.is_linear || dst_image->planes[0].surface.is_linear)
			xfer_fns->copy_image_l2t(cmd_buffer, &info, src_image, dst_image);
		else {
			if (xfer_fns->use_scanline_t2t(cmd_buffer, &info, src_image, dst_image))
				radv_transfer_cmd_copy_image_t2t_scanline(cmd_buffer, &info, src_image, dst_image);
			else
				xfer_fns->copy_image_t2t(cmd_buffer, &info, src_image, dst_image);
		}
	}
}

void radv_transfer_cmd_copy_buffer(struct radv_cmd_buffer *cmd_buffer,
				   struct radv_buffer *src_buffer,
				   struct radv_buffer *dst_buffer,
				   uint32_t region_count,
				   const VkBufferCopy *pregions)
{
	const struct radv_transfer_fns *xfer_fns = cmd_buffer->device->transfer_fns;
	uint64_t src_va, dst_va;

	src_va = radv_buffer_get_va(src_buffer->bo);
	dst_va = radv_buffer_get_va(dst_buffer->bo);
	src_va += src_buffer->offset;
	dst_va += dst_buffer->offset;

	for (uint32_t r = 0; r < region_count; r++) {
		VkDeviceSize bytes_to_copy = pregions[r].size;
		uint64_t this_src_va = src_va, this_dst_va = dst_va;

		this_src_va += pregions[r].srcOffset;
		this_dst_va += pregions[r].dstOffset;

		while (bytes_to_copy) {
			VkDeviceSize copied_bytes;
			copied_bytes = xfer_fns->emit_copy_buffer(cmd_buffer,
								  this_src_va,
								  this_dst_va,
								  bytes_to_copy);
			bytes_to_copy -= copied_bytes;
			this_src_va += copied_bytes;
			this_dst_va += copied_bytes;
		}
	}
}

void radv_transfer_cmd_fill_buffer(struct radv_cmd_buffer *cmd_buffer,
				   struct radv_buffer *dst_buffer,
				   VkDeviceSize dst_offset,
				   VkDeviceSize fill_size,
				   uint32_t data)
{
	const struct radv_transfer_fns *xfer_fns = cmd_buffer->device->transfer_fns;
	uint64_t dst_va = radv_buffer_get_va(dst_buffer->bo);
	VkDeviceSize bytes_to_copy = fill_size;
	dst_va += dst_buffer->offset;
	dst_va += dst_offset;

	while (bytes_to_copy) {
		VkDeviceSize copied_bytes;
		copied_bytes = xfer_fns->emit_fill_buffer(cmd_buffer, dst_va, bytes_to_copy, data);
		bytes_to_copy -= copied_bytes;
		dst_va += copied_bytes;
	}
}

void radv_transfer_cmd_update_buffer(struct radv_cmd_buffer *cmd_buffer,
				     struct radv_buffer *dst_buffer,
				     VkDeviceSize dst_offset,
				     VkDeviceSize data_size,
				     const void *pdata)
{
	uint64_t dst_va = radv_buffer_get_va(dst_buffer->bo);
	const struct radv_transfer_fns *xfer_fns = cmd_buffer->device->transfer_fns;
	dst_va += dst_buffer->offset;
	dst_va += dst_offset;

	xfer_fns->emit_update_buffer(cmd_buffer, dst_va,
				     data_size, pdata);
}
