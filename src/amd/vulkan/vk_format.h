/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 *
 * Based on u_format.h which is:
 * Copyright 2009-2010 Vmware, Inc.
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

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <assert.h>
#include <vulkan/vulkan.h>
enum vk_format_layout {
   /**
    * Formats with vk_format_block::width == vk_format_block::height == 1
    * that can be described as an ordinary data structure.
    */
   VK_FORMAT_LAYOUT_PLAIN = 0,

   /**
    * Formats with sub-sampled channels.
    *
    * This is for formats like YVYU where there is less than one sample per
    * pixel.
    */
   VK_FORMAT_LAYOUT_SUBSAMPLED = 3,

   /**
    * S3 Texture Compression formats.
    */
   VK_FORMAT_LAYOUT_S3TC = 4,

   /**
    * Red-Green Texture Compression formats.
    */
   VK_FORMAT_LAYOUT_RGTC = 5,

   /**
    * Ericsson Texture Compression
    */
   VK_FORMAT_LAYOUT_ETC = 6,

   /**
    * BC6/7 Texture Compression
    */
   VK_FORMAT_LAYOUT_BPTC = 7,

   /**
    * ASTC
    */
   VK_FORMAT_LAYOUT_ASTC = 8,

   /**
    * Everything else that doesn't fit in any of the above layouts.
    */
   VK_FORMAT_LAYOUT_OTHER = 9
};
   
struct vk_format_block
{
   /** Block width in pixels */
   unsigned width;
   
   /** Block height in pixels */
   unsigned height;

   /** Block size in bits */
   unsigned bits;
};
   
enum vk_format_type {
   VK_FORMAT_TYPE_VOID = 0,
   VK_FORMAT_TYPE_UNSIGNED = 1,
   VK_FORMAT_TYPE_SIGNED = 2,
   VK_FORMAT_TYPE_FIXED = 3,
   VK_FORMAT_TYPE_FLOAT = 4
};


enum vk_format_colorspace {
   VK_FORMAT_COLORSPACE_RGB = 0,
   VK_FORMAT_COLORSPACE_SRGB = 1,
   VK_FORMAT_COLORSPACE_YUV = 2,
   VK_FORMAT_COLORSPACE_ZS = 3
};

struct vk_format_channel_description {
   unsigned type:5;
   unsigned normalized:1;
   unsigned pure_integer:1;
   unsigned scaled:1;
   unsigned size:8;
   unsigned shift:16;
};

struct vk_format_description
{
   VkFormat format;
   const char *name;
   const char *short_name;

   struct vk_format_block block;
   enum vk_format_layout layout;

   unsigned nr_channels:3;
   unsigned is_array:1;
   unsigned is_bitmask:1;
   unsigned is_mixed:1;

   struct vk_format_channel_description channel[4];

   unsigned char swizzle[4];

   enum vk_format_colorspace colorspace;
};

extern const struct vk_format_description vk_format_description_table[];

const struct vk_format_description *vk_format_description(VkFormat format);

/**
 * Return total bits needed for the pixel format per block.
 */
static inline uint
vk_format_get_blocksizebits(VkFormat format)
{
   const struct vk_format_description *desc = vk_format_description(format);

   assert(desc);
   if (!desc) {
      return 0;
   }

   return desc->block.bits;
}

/**
 * Return bytes per block (not pixel) for the given format.
 */
static inline uint
vk_format_get_blocksize(VkFormat format)
{
   uint bits = vk_format_get_blocksizebits(format);
   uint bytes = bits / 8;

   assert(bits % 8 == 0);
   assert(bytes > 0);
   if (bytes == 0) {
      bytes = 1;
   }

   return bytes;
}

static inline uint
vk_format_get_blockwidth(VkFormat format)
{
   const struct vk_format_description *desc = vk_format_description(format);

   assert(desc);
   if (!desc) {
      return 1;
   }

   return desc->block.width;
}

static inline uint
vk_format_get_blockheight(VkFormat format)
{
   const struct vk_format_description *desc = vk_format_description(format);

   assert(desc);
   if (!desc) {
      return 1;
   }

   return desc->block.height;
}
   
/**
 * Return the index of the first non-void channel
 * -1 if no non-void channels
 */
static inline int
vk_format_get_first_non_void_channel(VkFormat format)
{
   const struct vk_format_description *desc = vk_format_description(format);
   int i;

   for (i = 0; i < 4; i++)
      if (desc->channel[i].type != VK_FORMAT_TYPE_VOID)
         break;

   if (i == 4)
       return -1;

   return i;
}

enum vk_swizzle {
   VK_SWIZZLE_X,
   VK_SWIZZLE_Y,
   VK_SWIZZLE_Z,
   VK_SWIZZLE_W,
   VK_SWIZZLE_0,
   VK_SWIZZLE_1,
   VK_SWIZZLE_NONE,
   VK_SWIZZLE_MAX, /**< Number of enums counter (must be last) */
};

  
static inline VkImageAspectFlags
vk_format_aspects(VkFormat format)
{
   switch (format) {
   case VK_FORMAT_UNDEFINED:
      return 0;

   case VK_FORMAT_S8_UINT:
      return VK_IMAGE_ASPECT_STENCIL_BIT;

   case VK_FORMAT_D16_UNORM_S8_UINT:
   case VK_FORMAT_D24_UNORM_S8_UINT:
   case VK_FORMAT_D32_SFLOAT_S8_UINT:
      return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;

   case VK_FORMAT_D16_UNORM:
   case VK_FORMAT_X8_D24_UNORM_PACK32:
   case VK_FORMAT_D32_SFLOAT:
      return VK_IMAGE_ASPECT_DEPTH_BIT;

   default:
      return VK_IMAGE_ASPECT_COLOR_BIT;
   }
}

static inline void vk_format_compose_swizzles(const unsigned char swz1[4],
					      const unsigned char swz2[4],
					      unsigned char dst[4])
{
   unsigned i;

   for (i = 0; i < 4; i++) {
      dst[i] = swz2[i] <= VK_SWIZZLE_W ?
               swz1[swz2[i]] : swz2[i];
   }
}

static inline bool
vk_format_is_compressed(VkFormat format)
{
   const struct vk_format_description *desc = vk_format_description(format);

   assert(desc);
   if (!desc) {
     return false;
   }

   switch (desc->layout) {
   case VK_FORMAT_LAYOUT_S3TC:
   case VK_FORMAT_LAYOUT_RGTC:
   case VK_FORMAT_LAYOUT_ETC:
   case VK_FORMAT_LAYOUT_BPTC:
   case VK_FORMAT_LAYOUT_ASTC:
      /* XXX add other formats in the future */
      return true;
   default:
      return false;
   }
}

static inline bool
vk_format_has_depth(const struct vk_format_description *desc)
{
   return desc->colorspace == VK_FORMAT_COLORSPACE_ZS &&
          desc->swizzle[0] != VK_SWIZZLE_NONE;
}

static inline bool
vk_format_has_stencil(const struct vk_format_description *desc)
{
   return desc->colorspace == VK_FORMAT_COLORSPACE_ZS &&
          desc->swizzle[1] != VK_SWIZZLE_NONE;
}

static inline bool
vk_format_is_depth_or_stencil(VkFormat format)
{
   const struct vk_format_description *desc = vk_format_description(format);

   assert(desc);
   if (!desc) {
      return false;
   }

   return vk_format_has_depth(desc) ||
          vk_format_has_stencil(desc);
}

static inline bool
vk_format_is_color(VkFormat format)
{
   return !vk_format_is_depth_or_stencil(format);
}

#ifdef __cplusplus
} // extern "C" {
#endif
