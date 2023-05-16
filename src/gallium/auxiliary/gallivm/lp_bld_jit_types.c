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
#include "gallivm/lp_bld_sample.h"
#include "gallivm/lp_bld_const.h"
#include "gallivm/lp_bld_debug.h"
#include "gallivm/lp_bld_ir_common.h"
#include "draw/draw_vertex_header.h"
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


/**
 * Fetch the specified member of the lp_jit_texture structure.
 * \param emit_load  if TRUE, emit the LLVM load instruction to actually
 *                   fetch the field's value.  Otherwise, just emit the
 *                   GEP code to address the field.
 *
 * @sa http://llvm.org/docs/GetElementPtr.html
 */
static LLVMValueRef
lp_build_llvm_texture_member(struct gallivm_state *gallivm,
                             LLVMTypeRef resources_type,
                             LLVMValueRef resources_ptr,
                             unsigned texture_unit,
                             LLVMValueRef texture_unit_offset,
                             unsigned member_index,
                             const char *member_name,
                             boolean emit_load,
                             LLVMTypeRef *out_type)
{
   LLVMBuilderRef builder = gallivm->builder;
   LLVMValueRef indices[4];

   assert(texture_unit < PIPE_MAX_SHADER_SAMPLER_VIEWS);

   /* resources[0] */
   indices[0] = lp_build_const_int32(gallivm, 0);
   /* resources[0].textures */
   indices[1] = lp_build_const_int32(gallivm, LP_JIT_RES_TEXTURES);
   /* resources[0].textures[unit] */
   indices[2] = lp_build_const_int32(gallivm, texture_unit);
   if (texture_unit_offset) {
      indices[2] = LLVMBuildAdd(gallivm->builder, indices[2],
                                texture_unit_offset, "");
      LLVMValueRef cond =
         LLVMBuildICmp(gallivm->builder, LLVMIntULT,
                       indices[2],
                       lp_build_const_int32(gallivm,
                                            PIPE_MAX_SHADER_SAMPLER_VIEWS), "");
      indices[2] = LLVMBuildSelect(gallivm->builder, cond, indices[2],
                                   lp_build_const_int32(gallivm,
                                                        texture_unit), "");
   }
   /* resources[0].textures[unit].member */
   indices[3] = lp_build_const_int32(gallivm, member_index);

   LLVMValueRef ptr =
      LLVMBuildGEP2(builder, resources_type, resources_ptr, indices, ARRAY_SIZE(indices), "");

   LLVMValueRef res;
   if (emit_load) {
      LLVMTypeRef tex_type = LLVMStructGetTypeAtIndex(resources_type, LP_JIT_RES_TEXTURES);
      LLVMTypeRef res_type = LLVMStructGetTypeAtIndex(LLVMGetElementType(tex_type), member_index);
      res = LLVMBuildLoad2(builder, res_type, ptr, "");
   } else
      res = ptr;

   if (out_type) {
      LLVMTypeRef tex_type = LLVMStructGetTypeAtIndex(resources_type, LP_JIT_RES_TEXTURES);
      LLVMTypeRef res_type = LLVMStructGetTypeAtIndex(LLVMGetElementType(tex_type), member_index);
      *out_type = res_type;
   }

   lp_build_name(res, "resources.texture%u.%s", texture_unit, member_name);

   return res;
}


/**
 * Helper macro to instantiate the functions that generate the code to
 * fetch the members of lp_jit_texture to fulfill the sampler code
 * generator requests.
 *
 * This complexity is the price we have to pay to keep the texture
 * sampler code generator a reusable module without dependencies to
 * llvmpipe internals.
 */
#define LP_BUILD_LLVM_TEXTURE_MEMBER(_name, _index, _emit_load)  \
   static LLVMValueRef \
   lp_build_llvm_texture_##_name(struct gallivm_state *gallivm, \
                                 LLVMTypeRef resources_type,    \
                                 LLVMValueRef resources_ptr,    \
                                 unsigned texture_unit,         \
                                 LLVMValueRef texture_unit_offset)      \
   { \
      return lp_build_llvm_texture_member(gallivm, resources_type, resources_ptr, \
                                          texture_unit, texture_unit_offset, \
                                          _index, #_name, _emit_load, NULL ); \
   }

#define LP_BUILD_LLVM_TEXTURE_MEMBER_OUTTYPE(_name, _index, _emit_load)  \
   static LLVMValueRef \
   lp_build_llvm_texture_##_name(struct gallivm_state *gallivm, \
                           LLVMTypeRef resources_type, \
                           LLVMValueRef resources_ptr, \
                           unsigned texture_unit,    \
                           LLVMValueRef texture_unit_offset, \
                           LLVMTypeRef *out_type)            \
   { \
      return lp_build_llvm_texture_member(gallivm, resources_type, resources_ptr, \
                                          texture_unit, texture_unit_offset, \
                                          _index, #_name, _emit_load, out_type ); \
   }

LP_BUILD_LLVM_TEXTURE_MEMBER(width,      LP_JIT_TEXTURE_WIDTH, TRUE)
LP_BUILD_LLVM_TEXTURE_MEMBER(height,     LP_JIT_TEXTURE_HEIGHT, TRUE)
LP_BUILD_LLVM_TEXTURE_MEMBER(depth,      LP_JIT_TEXTURE_DEPTH, TRUE)
LP_BUILD_LLVM_TEXTURE_MEMBER(first_level, LP_JIT_TEXTURE_FIRST_LEVEL, TRUE)
LP_BUILD_LLVM_TEXTURE_MEMBER(last_level, LP_JIT_TEXTURE_LAST_LEVEL, TRUE)
LP_BUILD_LLVM_TEXTURE_MEMBER(base_ptr,   LP_JIT_TEXTURE_BASE, TRUE)
LP_BUILD_LLVM_TEXTURE_MEMBER_OUTTYPE(row_stride, LP_JIT_TEXTURE_ROW_STRIDE, FALSE)
LP_BUILD_LLVM_TEXTURE_MEMBER_OUTTYPE(img_stride, LP_JIT_TEXTURE_IMG_STRIDE, FALSE)
LP_BUILD_LLVM_TEXTURE_MEMBER_OUTTYPE(mip_offsets, LP_JIT_TEXTURE_MIP_OFFSETS, FALSE)
LP_BUILD_LLVM_TEXTURE_MEMBER(num_samples, LP_JIT_TEXTURE_NUM_SAMPLES, TRUE)
LP_BUILD_LLVM_TEXTURE_MEMBER(sample_stride, LP_JIT_TEXTURE_SAMPLE_STRIDE, TRUE)

/**
 * Fetch the specified member of the lp_jit_sampler structure.
 * \param emit_load  if TRUE, emit the LLVM load instruction to actually
 *                   fetch the field's value.  Otherwise, just emit the
 *                   GEP code to address the field.
 *
 * @sa http://llvm.org/docs/GetElementPtr.html
 */
static LLVMValueRef
lp_build_llvm_sampler_member(struct gallivm_state *gallivm,
                             LLVMTypeRef resources_type,
                             LLVMValueRef resources_ptr,
                             unsigned sampler_unit,
                             unsigned member_index,
                             const char *member_name,
                             boolean emit_load)
{
   LLVMBuilderRef builder = gallivm->builder;
   LLVMValueRef indices[4];

   assert(sampler_unit < PIPE_MAX_SAMPLERS);

   /* resources[0] */
   indices[0] = lp_build_const_int32(gallivm, 0);
   /* resources[0].samplers */
   indices[1] = lp_build_const_int32(gallivm, LP_JIT_RES_SAMPLERS);
   /* resources[0].samplers[unit] */
   indices[2] = lp_build_const_int32(gallivm, sampler_unit);
   /* resources[0].samplers[unit].member */
   indices[3] = lp_build_const_int32(gallivm, member_index);

   LLVMValueRef ptr =
      LLVMBuildGEP2(builder, resources_type, resources_ptr, indices, ARRAY_SIZE(indices), "");

   LLVMValueRef res;
   if (emit_load) {
      LLVMTypeRef samp_type = LLVMStructGetTypeAtIndex(resources_type, LP_JIT_RES_SAMPLERS);
      LLVMTypeRef res_type = LLVMStructGetTypeAtIndex(LLVMGetElementType(samp_type), member_index);
      res = LLVMBuildLoad2(builder, res_type, ptr, "");
   } else
      res = ptr;

   lp_build_name(res, "resources.sampler%u.%s", sampler_unit, member_name);

   return res;
}


#define LP_BUILD_LLVM_SAMPLER_MEMBER(_name, _index, _emit_load)   \
   static LLVMValueRef                                            \
   lp_build_llvm_sampler_##_name( struct gallivm_state *gallivm,  \
                                  LLVMTypeRef resources_type,     \
                                  LLVMValueRef resources_ptr,     \
                                  unsigned sampler_unit)          \
   {                                                              \
      return lp_build_llvm_sampler_member(gallivm, resources_type, resources_ptr, \
                                          sampler_unit, _index, #_name, _emit_load ); \
   }


LP_BUILD_LLVM_SAMPLER_MEMBER(min_lod,    LP_JIT_SAMPLER_MIN_LOD, TRUE)
LP_BUILD_LLVM_SAMPLER_MEMBER(max_lod,    LP_JIT_SAMPLER_MAX_LOD, TRUE)
LP_BUILD_LLVM_SAMPLER_MEMBER(lod_bias,   LP_JIT_SAMPLER_LOD_BIAS, TRUE)
LP_BUILD_LLVM_SAMPLER_MEMBER(border_color, LP_JIT_SAMPLER_BORDER_COLOR, FALSE)
LP_BUILD_LLVM_SAMPLER_MEMBER(max_aniso, LP_JIT_SAMPLER_MAX_ANISO, TRUE)

/**
 * Fetch the specified member of the lp_jit_image structure.
 * \param emit_load  if TRUE, emit the LLVM load instruction to actually
 *                   fetch the field's value.  Otherwise, just emit the
 *                   GEP code to address the field.
 *
 * @sa http://llvm.org/docs/GetElementPtr.html
 */
static LLVMValueRef
lp_build_llvm_image_member(struct gallivm_state *gallivm,
                           LLVMTypeRef resources_type,
                           LLVMValueRef resources_ptr,
                           unsigned image_unit,
                           LLVMValueRef image_unit_offset,
                           unsigned member_index,
                           const char *member_name,
                           boolean emit_load)
{
   LLVMBuilderRef builder = gallivm->builder;
   LLVMValueRef indices[4];

   assert(image_unit < PIPE_MAX_SHADER_IMAGES);

   /* resources[0] */
   indices[0] = lp_build_const_int32(gallivm, 0);
   /* resources[0].images */
   indices[1] = lp_build_const_int32(gallivm, LP_JIT_RES_IMAGES);
   /* resources[0].images[unit] */
   indices[2] = lp_build_const_int32(gallivm, image_unit);
   if (image_unit_offset) {
      indices[2] = LLVMBuildAdd(gallivm->builder, indices[2], image_unit_offset, "");
      LLVMValueRef cond = LLVMBuildICmp(gallivm->builder, LLVMIntULT, indices[2], lp_build_const_int32(gallivm, PIPE_MAX_SHADER_IMAGES), "");
      indices[2] = LLVMBuildSelect(gallivm->builder, cond, indices[2], lp_build_const_int32(gallivm, image_unit), "");
   }
   /* resources[0].images[unit].member */
   indices[3] = lp_build_const_int32(gallivm, member_index);

   LLVMValueRef ptr =
      LLVMBuildGEP2(builder, resources_type, resources_ptr, indices, ARRAY_SIZE(indices), "");

   LLVMValueRef res;
   if (emit_load) {
      LLVMTypeRef img_type = LLVMStructGetTypeAtIndex(resources_type, LP_JIT_RES_IMAGES);
      LLVMTypeRef res_type = LLVMStructGetTypeAtIndex(LLVMGetElementType(img_type), member_index);
      res = LLVMBuildLoad2(builder, res_type, ptr, "");
   } else
      res = ptr;

   lp_build_name(res, "resources.image%u.%s", image_unit, member_name);

   return res;
}

/**
 * Helper macro to instantiate the functions that generate the code to
 * fetch the members of lp_jit_image to fulfill the sampler code
 * generator requests.
 *
 * This complexity is the price we have to pay to keep the image
 * sampler code generator a reusable module without dependencies to
 * llvmpipe internals.
 */
#define LP_BUILD_LLVM_IMAGE_MEMBER(_name, _index, _emit_load)  \
   static LLVMValueRef \
   lp_build_llvm_image_##_name( struct gallivm_state *gallivm,               \
                                LLVMTypeRef resources_type,             \
                                LLVMValueRef resources_ptr,             \
                                unsigned image_unit, LLVMValueRef image_unit_offset) \
   { \
      return lp_build_llvm_image_member(gallivm, resources_type, resources_ptr, \
                                  image_unit, image_unit_offset, \
                                  _index, #_name, _emit_load );  \
   }

#define LP_BUILD_LLVM_IMAGE_MEMBER_OUTTYPE(_name, _index, _emit_load)  \
   static LLVMValueRef \
   lp_build_llvm_image_##_name( struct gallivm_state *gallivm,               \
                                LLVMTypeRef resources_type,             \
                                LLVMValueRef resources_ptr,             \
                                unsigned image_unit, LLVMValueRef image_unit_offset, \
                                LLVMTypeRef *out_type)                  \
   { \
      assert(!out_type);                                                \
      return lp_build_llvm_image_member(gallivm, resources_type, resources_ptr,    \
                                        image_unit, image_unit_offset,  \
                                        _index, #_name, _emit_load );   \
   }

LP_BUILD_LLVM_IMAGE_MEMBER(width,      LP_JIT_IMAGE_WIDTH, TRUE)
LP_BUILD_LLVM_IMAGE_MEMBER(height,     LP_JIT_IMAGE_HEIGHT, TRUE)
LP_BUILD_LLVM_IMAGE_MEMBER(depth,      LP_JIT_IMAGE_DEPTH, TRUE)
LP_BUILD_LLVM_IMAGE_MEMBER(base_ptr,   LP_JIT_IMAGE_BASE, TRUE)
LP_BUILD_LLVM_IMAGE_MEMBER_OUTTYPE(row_stride, LP_JIT_IMAGE_ROW_STRIDE, TRUE)
LP_BUILD_LLVM_IMAGE_MEMBER_OUTTYPE(img_stride, LP_JIT_IMAGE_IMG_STRIDE, TRUE)
LP_BUILD_LLVM_IMAGE_MEMBER(num_samples, LP_JIT_IMAGE_NUM_SAMPLES, TRUE)
LP_BUILD_LLVM_IMAGE_MEMBER(sample_stride, LP_JIT_IMAGE_SAMPLE_STRIDE, TRUE)

void
lp_build_jit_fill_sampler_dynamic_state(struct lp_sampler_dynamic_state *state)
{
   state->width = lp_build_llvm_texture_width;
   state->height = lp_build_llvm_texture_height;
   state->depth = lp_build_llvm_texture_depth;
   state->first_level = lp_build_llvm_texture_first_level;
   state->last_level = lp_build_llvm_texture_last_level;
   state->base_ptr = lp_build_llvm_texture_base_ptr;
   state->row_stride = lp_build_llvm_texture_row_stride;
   state->img_stride = lp_build_llvm_texture_img_stride;
   state->mip_offsets = lp_build_llvm_texture_mip_offsets;
   state->num_samples = lp_build_llvm_texture_num_samples;
   state->sample_stride = lp_build_llvm_texture_sample_stride;

   state->min_lod = lp_build_llvm_sampler_min_lod;
   state->max_lod = lp_build_llvm_sampler_max_lod;
   state->lod_bias = lp_build_llvm_sampler_lod_bias;
   state->border_color = lp_build_llvm_sampler_border_color;
   state->max_aniso = lp_build_llvm_sampler_max_aniso;
}

void
lp_build_jit_fill_image_dynamic_state(struct lp_sampler_dynamic_state *state)
{
   state->width = lp_build_llvm_image_width;
   state->height = lp_build_llvm_image_height;

   state->depth = lp_build_llvm_image_depth;
   state->base_ptr = lp_build_llvm_image_base_ptr;
   state->row_stride = lp_build_llvm_image_row_stride;
   state->img_stride = lp_build_llvm_image_img_stride;
   state->num_samples = lp_build_llvm_image_num_samples;
   state->sample_stride = lp_build_llvm_image_sample_stride;
}

/**
 * Create LLVM type for struct vertex_header;
 */
LLVMTypeRef
lp_build_create_jit_vertex_header_type(struct gallivm_state *gallivm, int data_elems)
{
   LLVMTargetDataRef target = gallivm->target;
   LLVMTypeRef elem_types[3];
   LLVMTypeRef vertex_header;
   char struct_name[24];

   snprintf(struct_name, 23, "vertex_header%d", data_elems);

   elem_types[LP_JIT_VERTEX_HEADER_VERTEX_ID]  = LLVMIntTypeInContext(gallivm->context, 32);
   elem_types[LP_JIT_VERTEX_HEADER_CLIP_POS]  = LLVMArrayType(LLVMFloatTypeInContext(gallivm->context), 4);
   elem_types[LP_JIT_VERTEX_HEADER_DATA]  = LLVMArrayType(elem_types[1], data_elems);

   vertex_header = LLVMStructTypeInContext(gallivm->context, elem_types,
                                           ARRAY_SIZE(elem_types), 0);

   /* these are bit-fields and we can't take address of them
      LP_CHECK_MEMBER_OFFSET(struct vertex_header, clipmask,
      target, vertex_header,
      LP_JIT_VERTEX_HEADER_CLIPMASK);
      LP_CHECK_MEMBER_OFFSET(struct vertex_header, edgeflag,
      target, vertex_header,
      LP_JIT_VERTEX_HEADER_EDGEFLAG);
      LP_CHECK_MEMBER_OFFSET(struct vertex_header, pad,
      target, vertex_header,
      LP_JIT_VERTEX_HEADER_PAD);
      LP_CHECK_MEMBER_OFFSET(struct vertex_header, vertex_id,
      target, vertex_header,
      LP_JIT_VERTEX_HEADER_VERTEX_ID);
   */
   (void) target; /* silence unused var warning for non-debug build */
   LP_CHECK_MEMBER_OFFSET(struct vertex_header, clip_pos,
                          target, vertex_header,
                          LP_JIT_VERTEX_HEADER_CLIP_POS);
   LP_CHECK_MEMBER_OFFSET(struct vertex_header, data,
                          target, vertex_header,
                          LP_JIT_VERTEX_HEADER_DATA);

   assert(LLVMABISizeOfType(target, vertex_header) ==
          offsetof(struct vertex_header, data[data_elems]));

   return vertex_header;
}
