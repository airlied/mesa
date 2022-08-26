/*
 * Copyright 2022 Red Hat.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "pipe/p_compiler.h"
#include "gallivm/lp_bld.h"
#include "gallivm/lp_bld_init.h"
#include "gallivm/lp_bld_struct.h"
#include "gallivm/lp_bld_const.h"
#include "gallivm/lp_bld_debug.h"
#include "gallivm/lp_bld_ir_common.h"
#include "lp_bld_jit_types.h"


static LLVMTypeRef
lp_build_create_jit_buffer_type(struct gallivm_state *gallivm)
{
   LLVMContextRef lc = gallivm->context;
   LLVMTypeRef buffer_type;
   LLVMTypeRef elem_types[LP_JIT_BUFFER_NUM_FIELDS];

   elem_types[LP_JIT_BUFFER_BASE] = LLVMPointerType(LLVMInt32TypeInContext(lc), 0);
   elem_types[LP_JIT_BUFFER_NUM_ELEMENTS] = LLVMInt32TypeInContext(lc);

   buffer_type = LLVMStructTypeInContext(lc, elem_types,
                                         ARRAY_SIZE(elem_types), 0);

   LP_CHECK_MEMBER_OFFSET(struct lp_jit_buffer, f,
                          gallivm->target, buffer_type,
                          LP_JIT_BUFFER_BASE);

   LP_CHECK_MEMBER_OFFSET(struct lp_jit_buffer, num_elements,
                          gallivm->target, buffer_type,
                          LP_JIT_BUFFER_NUM_ELEMENTS);
   return buffer_type;
}

static LLVMValueRef
lp_llvm_buffer_member(struct gallivm_state *gallivm,
                      LLVMValueRef buffers_ptr,
                      LLVMValueRef buffers_offset,
                      unsigned buffers_limit,
                      unsigned member_index,
                      const char *member_name)
{
   LLVMBuilderRef builder = gallivm->builder;
   LLVMValueRef indices[3];

   indices[0] = lp_build_const_int32(gallivm, 0);
   LLVMValueRef cond = LLVMBuildICmp(gallivm->builder, LLVMIntULT, buffers_offset, lp_build_const_int32(gallivm, buffers_limit), "");
   indices[1] = LLVMBuildSelect(gallivm->builder, cond, buffers_offset, lp_build_const_int32(gallivm, 0), "");
   indices[2] = lp_build_const_int32(gallivm, member_index);

   LLVMTypeRef buffer_type = lp_build_create_jit_buffer_type(gallivm);
   LLVMTypeRef buffers_type = LLVMArrayType(buffer_type, buffers_limit);
   LLVMValueRef ptr = LLVMBuildGEP2(builder, buffers_type, buffers_ptr, indices, ARRAY_SIZE(indices), "");

   LLVMTypeRef res_type = LLVMStructGetTypeAtIndex(buffer_type, member_index);
   LLVMValueRef res = LLVMBuildLoad2(builder, res_type, ptr, "");

   lp_build_name(res, "buffer.%s", member_name);

   return res;
}

LLVMValueRef
lp_llvm_buffer_base(struct gallivm_state *gallivm,
                    LLVMValueRef buffers_ptr, LLVMValueRef buffers_offset, unsigned buffers_limit)
{
   return lp_llvm_buffer_member(gallivm, buffers_ptr, buffers_offset, buffers_limit, LP_JIT_BUFFER_BASE, "base");
}

LLVMValueRef
lp_llvm_buffer_num_elements(struct gallivm_state *gallivm,
                    LLVMValueRef buffers_ptr, LLVMValueRef buffers_offset, unsigned buffers_limit)
{
   return lp_llvm_buffer_member(gallivm, buffers_ptr, buffers_offset, buffers_limit, LP_JIT_BUFFER_NUM_ELEMENTS, "num_elements");
}

static LLVMTypeRef
lp_build_create_jit_texture_type(struct gallivm_state *gallivm)
{
   LLVMContextRef lc = gallivm->context;
   LLVMTypeRef texture_type;
   LLVMTypeRef elem_types[LP_JIT_TEXTURE_NUM_FIELDS];

   /* struct lp_jit_texture */
   elem_types[LP_JIT_TEXTURE_WIDTH]  =
   elem_types[LP_JIT_TEXTURE_HEIGHT] =
   elem_types[LP_JIT_TEXTURE_DEPTH] =
   elem_types[LP_JIT_TEXTURE_NUM_SAMPLES] =
   elem_types[LP_JIT_TEXTURE_SAMPLE_STRIDE] =
   elem_types[LP_JIT_TEXTURE_FIRST_LEVEL] =
   elem_types[LP_JIT_TEXTURE_LAST_LEVEL] = LLVMInt32TypeInContext(lc);
   elem_types[LP_JIT_TEXTURE_BASE] = LLVMPointerType(LLVMInt8TypeInContext(lc), 0);
   elem_types[LP_JIT_TEXTURE_ROW_STRIDE] =
   elem_types[LP_JIT_TEXTURE_IMG_STRIDE] =
   elem_types[LP_JIT_TEXTURE_MIP_OFFSETS] =
      LLVMArrayType(LLVMInt32TypeInContext(lc), PIPE_MAX_TEXTURE_LEVELS);

   texture_type = LLVMStructTypeInContext(lc, elem_types,
                                          ARRAY_SIZE(elem_types), 0);

   LP_CHECK_MEMBER_OFFSET(struct lp_jit_texture, width,
                          gallivm->target, texture_type,
                          LP_JIT_TEXTURE_WIDTH);
   LP_CHECK_MEMBER_OFFSET(struct lp_jit_texture, height,
                          gallivm->target, texture_type,
                          LP_JIT_TEXTURE_HEIGHT);
   LP_CHECK_MEMBER_OFFSET(struct lp_jit_texture, depth,
                          gallivm->target, texture_type,
                          LP_JIT_TEXTURE_DEPTH);
   LP_CHECK_MEMBER_OFFSET(struct lp_jit_texture, base,
                          gallivm->target, texture_type,
                          LP_JIT_TEXTURE_BASE);
   LP_CHECK_MEMBER_OFFSET(struct lp_jit_texture, row_stride,
                          gallivm->target, texture_type,
                          LP_JIT_TEXTURE_ROW_STRIDE);
   LP_CHECK_MEMBER_OFFSET(struct lp_jit_texture, img_stride,
                          gallivm->target, texture_type,
                          LP_JIT_TEXTURE_IMG_STRIDE);
   LP_CHECK_MEMBER_OFFSET(struct lp_jit_texture, first_level,
                          gallivm->target, texture_type,
                          LP_JIT_TEXTURE_FIRST_LEVEL);
   LP_CHECK_MEMBER_OFFSET(struct lp_jit_texture, last_level,
                          gallivm->target, texture_type,
                          LP_JIT_TEXTURE_LAST_LEVEL);
   LP_CHECK_MEMBER_OFFSET(struct lp_jit_texture, mip_offsets,
                          gallivm->target, texture_type,
                          LP_JIT_TEXTURE_MIP_OFFSETS);
   LP_CHECK_MEMBER_OFFSET(struct lp_jit_texture, num_samples,
                          gallivm->target, texture_type,
                          LP_JIT_TEXTURE_NUM_SAMPLES);
   LP_CHECK_MEMBER_OFFSET(struct lp_jit_texture, sample_stride,
                          gallivm->target, texture_type,
                          LP_JIT_TEXTURE_SAMPLE_STRIDE);
   LP_CHECK_STRUCT_SIZE(struct lp_jit_texture,
                        gallivm->target, texture_type);
   return texture_type;
}

static LLVMTypeRef
lp_build_create_jit_sampler_type(struct gallivm_state *gallivm)
{
   LLVMContextRef lc = gallivm->context;
   LLVMTypeRef sampler_type;
   LLVMTypeRef elem_types[LP_JIT_SAMPLER_NUM_FIELDS];
   elem_types[LP_JIT_SAMPLER_MIN_LOD] =
   elem_types[LP_JIT_SAMPLER_MAX_LOD] =
   elem_types[LP_JIT_SAMPLER_LOD_BIAS] =
   elem_types[LP_JIT_SAMPLER_MAX_ANISO] = LLVMFloatTypeInContext(lc);
   elem_types[LP_JIT_SAMPLER_BORDER_COLOR] =
      LLVMArrayType(LLVMFloatTypeInContext(lc), 4);

   sampler_type = LLVMStructTypeInContext(lc, elem_types,
                                          ARRAY_SIZE(elem_types), 0);

   LP_CHECK_MEMBER_OFFSET(struct lp_jit_sampler, min_lod,
                          gallivm->target, sampler_type,
                          LP_JIT_SAMPLER_MIN_LOD);
   LP_CHECK_MEMBER_OFFSET(struct lp_jit_sampler, max_lod,
                          gallivm->target, sampler_type,
                          LP_JIT_SAMPLER_MAX_LOD);
   LP_CHECK_MEMBER_OFFSET(struct lp_jit_sampler, lod_bias,
                          gallivm->target, sampler_type,
                          LP_JIT_SAMPLER_LOD_BIAS);
   LP_CHECK_MEMBER_OFFSET(struct lp_jit_sampler, border_color,
                          gallivm->target, sampler_type,
                          LP_JIT_SAMPLER_BORDER_COLOR);
   LP_CHECK_MEMBER_OFFSET(struct lp_jit_sampler, max_aniso,
                          gallivm->target, sampler_type,
                          LP_JIT_SAMPLER_MAX_ANISO);
   LP_CHECK_STRUCT_SIZE(struct lp_jit_sampler,
                        gallivm->target, sampler_type);
   return sampler_type;
}

static LLVMTypeRef
lp_build_create_jit_image_type(struct gallivm_state *gallivm)
{
   LLVMContextRef lc = gallivm->context;
   LLVMTypeRef image_type;
   LLVMTypeRef elem_types[LP_JIT_IMAGE_NUM_FIELDS];
   elem_types[LP_JIT_IMAGE_WIDTH] =
   elem_types[LP_JIT_IMAGE_HEIGHT] =
   elem_types[LP_JIT_IMAGE_DEPTH] = LLVMInt32TypeInContext(lc);
   elem_types[LP_JIT_IMAGE_BASE] = LLVMPointerType(LLVMInt8TypeInContext(lc), 0);
   elem_types[LP_JIT_IMAGE_ROW_STRIDE] =
   elem_types[LP_JIT_IMAGE_IMG_STRIDE] =
   elem_types[LP_JIT_IMAGE_NUM_SAMPLES] =
   elem_types[LP_JIT_IMAGE_SAMPLE_STRIDE] = LLVMInt32TypeInContext(lc);

   image_type = LLVMStructTypeInContext(lc, elem_types,
                                        ARRAY_SIZE(elem_types), 0);
   LP_CHECK_MEMBER_OFFSET(struct lp_jit_image, width,
                          gallivm->target, image_type,
                          LP_JIT_IMAGE_WIDTH);
   LP_CHECK_MEMBER_OFFSET(struct lp_jit_image, height,
                          gallivm->target, image_type,
                          LP_JIT_IMAGE_HEIGHT);
   LP_CHECK_MEMBER_OFFSET(struct lp_jit_image, depth,
                          gallivm->target, image_type,
                          LP_JIT_IMAGE_DEPTH);
   LP_CHECK_MEMBER_OFFSET(struct lp_jit_image, base,
                          gallivm->target, image_type,
                          LP_JIT_IMAGE_BASE);
   LP_CHECK_MEMBER_OFFSET(struct lp_jit_image, row_stride,
                          gallivm->target, image_type,
                          LP_JIT_IMAGE_ROW_STRIDE);
   LP_CHECK_MEMBER_OFFSET(struct lp_jit_image, img_stride,
                          gallivm->target, image_type,
                          LP_JIT_IMAGE_IMG_STRIDE);
   LP_CHECK_MEMBER_OFFSET(struct lp_jit_image, num_samples,
                          gallivm->target, image_type,
                          LP_JIT_IMAGE_NUM_SAMPLES);
   LP_CHECK_MEMBER_OFFSET(struct lp_jit_image, sample_stride,
                          gallivm->target, image_type,
                          LP_JIT_IMAGE_SAMPLE_STRIDE);
   return image_type;
}

LLVMTypeRef
lp_build_jit_resources_type(struct gallivm_state *gallivm)
{
   LLVMTypeRef elem_types[LP_JIT_RES_COUNT];
   LLVMTypeRef resources_type;
   LLVMTypeRef texture_type, sampler_type, image_type, buffer_type;

   buffer_type = lp_build_create_jit_buffer_type(gallivm);
   texture_type = lp_build_create_jit_texture_type(gallivm);
   sampler_type = lp_build_create_jit_sampler_type(gallivm);
   image_type = lp_build_create_jit_image_type(gallivm);
   elem_types[LP_JIT_RES_CONSTANTS] = LLVMArrayType(buffer_type,
                                                    LP_MAX_TGSI_CONST_BUFFERS);
   elem_types[LP_JIT_RES_SSBOS] =
      LLVMArrayType(buffer_type, LP_MAX_TGSI_SHADER_BUFFERS);
   elem_types[LP_JIT_RES_TEXTURES] = LLVMArrayType(texture_type,
                                                   PIPE_MAX_SHADER_SAMPLER_VIEWS);
   elem_types[LP_JIT_RES_SAMPLERS] = LLVMArrayType(sampler_type,
                                                   PIPE_MAX_SAMPLERS);
   elem_types[LP_JIT_RES_IMAGES] = LLVMArrayType(image_type,
                                                 PIPE_MAX_SHADER_IMAGES);
   elem_types[LP_JIT_RES_ANISO_FILTER_TABLE] = LLVMPointerType(LLVMFloatTypeInContext(gallivm->context), 0);

   resources_type = LLVMStructTypeInContext(gallivm->context, elem_types,
                                            ARRAY_SIZE(elem_types), 0);

   LP_CHECK_MEMBER_OFFSET(struct lp_jit_resources, constants,
                          gallivm->target, resources_type,
                          LP_JIT_RES_CONSTANTS);
   LP_CHECK_MEMBER_OFFSET(struct lp_jit_resources, ssbos,
                          gallivm->target, resources_type,
                          LP_JIT_RES_SSBOS);
   LP_CHECK_MEMBER_OFFSET(struct lp_jit_resources, textures,
                          gallivm->target, resources_type,
                          LP_JIT_RES_TEXTURES);
   LP_CHECK_MEMBER_OFFSET(struct lp_jit_resources, samplers,
                          gallivm->target, resources_type,
                          LP_JIT_RES_SAMPLERS);
   LP_CHECK_MEMBER_OFFSET(struct lp_jit_resources, images,
                          gallivm->target, resources_type,
                          LP_JIT_RES_IMAGES);
   LP_CHECK_MEMBER_OFFSET(struct lp_jit_resources, aniso_filter_table,
                          gallivm->target, resources_type,
                          LP_JIT_RES_ANISO_FILTER_TABLE);

   return resources_type;
}
