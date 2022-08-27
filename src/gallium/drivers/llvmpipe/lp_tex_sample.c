/**************************************************************************
 *
 * Copyright 2009 VMware, Inc.
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

/**
 * Texture sampling code generation
 *
 * This file is nothing more than ugly glue between three largely independent
 * entities:
 * - TGSI -> LLVM translation (i.e., lp_build_tgsi_soa)
 * - texture sampling code generation (i.e., lp_build_sample_soa)
 * - LLVM pipe driver
 *
 * All interesting code is in the functions mentioned above. There is really
 * nothing to see here.
 *
 * @author Jose Fonseca <jfonseca@vmware.com>
 */

#include "pipe/p_defines.h"
#include "pipe/p_shader_tokens.h"
#include "gallivm/lp_bld_debug.h"
#include "gallivm/lp_bld_const.h"
#include "gallivm/lp_bld_type.h"
#include "gallivm/lp_bld_sample.h"
#include "gallivm/lp_bld_tgsi.h"
#include "lp_jit.h"
#include "lp_tex_sample.h"
#include "lp_state_fs.h"
#include "lp_debug.h"


/**
 * This provides the bridge between the sampler state store in
 * lp_jit_context and lp_jit_texture and the sampler code
 * generator. It provides the texture layout information required by
 * the texture sampler code generator in terms of the state stored in
 * lp_jit_context and lp_jit_texture in runtime.
 */
struct llvmpipe_sampler_dynamic_state
{
   struct lp_sampler_dynamic_state base;

   const struct lp_sampler_static_state *static_state;
};


/**
 * This is the bridge between our sampler and the TGSI translator.
 */
struct lp_llvm_sampler_soa
{
   struct lp_build_sampler_soa base;

   struct llvmpipe_sampler_dynamic_state dynamic_state;
   unsigned nr_samplers;
};


struct llvmpipe_image_dynamic_state
{
   struct lp_sampler_dynamic_state base;

   const struct lp_image_static_state *static_state;
};


/**
 * This is the bridge between our images and the TGSI translator.
 */
struct lp_llvm_image_soa
{
   struct lp_build_image_soa base;

   struct llvmpipe_image_dynamic_state dynamic_state;
   unsigned nr_images;
};

#if LP_USE_TEXTURE_CACHE
static LLVMValueRef
lp_llvm_texture_cache_ptr(struct gallivm_state *gallivm,
                          LLVMTypeRef thread_data_type,
                          LLVMValueRef thread_data_ptr,
                          unsigned unit)
{
   /* We use the same cache for all units */
   (void)unit;

   return lp_jit_thread_data_cache(gallivm, thread_data_type, thread_data_ptr);
}
#endif

/**
 * Fetch filtered values from texture.
 * The 'texel' parameter returns four vectors corresponding to R, G, B, A.
 */
static void
lp_llvm_sampler_soa_emit_fetch_texel(const struct lp_build_sampler_soa *base,
                                     struct gallivm_state *gallivm,
                                     const struct lp_sampler_params *params)
{
   struct lp_llvm_sampler_soa *sampler = (struct lp_llvm_sampler_soa *)base;
   const unsigned texture_index = params->texture_index;
   const unsigned sampler_index = params->sampler_index;

   assert(sampler_index < PIPE_MAX_SAMPLERS);
   assert(texture_index < PIPE_MAX_SHADER_SAMPLER_VIEWS);

   if (LP_PERF & PERF_NO_TEX) {
      lp_build_sample_nop(gallivm, params->type, params->coords, params->texel);
      return;
   }

   if (params->texture_index_offset) {
      LLVMValueRef unit =
         LLVMBuildAdd(gallivm->builder, params->texture_index_offset,
                      lp_build_const_int32(gallivm, texture_index), "");

      struct lp_build_sample_array_switch switch_info;
      memset(&switch_info, 0, sizeof(switch_info));
      lp_build_sample_array_init_soa(&switch_info, gallivm, params, unit,
                                     0, sampler->nr_samplers);
      // build the switch cases
      for (unsigned i = 0; i < sampler->nr_samplers; i++) {
         lp_build_sample_array_case_soa(&switch_info, i,
                                        &sampler->dynamic_state.static_state[i].texture_state,
                                        &sampler->dynamic_state.static_state[i].sampler_state,
                                        &sampler->dynamic_state.base);
      }
      lp_build_sample_array_fini_soa(&switch_info);
   } else {
      lp_build_sample_soa(&sampler->dynamic_state.static_state[texture_index].texture_state,
                          &sampler->dynamic_state.static_state[sampler_index].sampler_state,
                          &sampler->dynamic_state.base,
                          gallivm, params);
   }
}


/**
 * Fetch the texture size.
 */
static void
lp_llvm_sampler_soa_emit_size_query(const struct lp_build_sampler_soa *base,
                                    struct gallivm_state *gallivm,
                                    const struct lp_sampler_size_query_params *params)
{
   struct lp_llvm_sampler_soa *sampler = (struct lp_llvm_sampler_soa *)base;

   assert(params->texture_unit < PIPE_MAX_SHADER_SAMPLER_VIEWS);

   lp_build_size_query_soa(gallivm,
                           &sampler->dynamic_state.static_state[params->texture_unit].texture_state,
                           &sampler->dynamic_state.base,
                           params);
}


struct lp_build_sampler_soa *
lp_llvm_sampler_soa_create(const struct lp_sampler_static_state *static_state,
                           unsigned nr_samplers)
{
   assert(static_state);

   struct lp_llvm_sampler_soa *sampler = CALLOC_STRUCT(lp_llvm_sampler_soa);
   if (!sampler)
      return NULL;

   sampler->base.emit_tex_sample = lp_llvm_sampler_soa_emit_fetch_texel;
   sampler->base.emit_size_query = lp_llvm_sampler_soa_emit_size_query;

   lp_build_jit_fill_sampler_dynamic_state(&sampler->dynamic_state.base);

#if LP_USE_TEXTURE_CACHE
   sampler->dynamic_state.base.cache_ptr = lp_llvm_texture_cache_ptr;
#endif

   sampler->dynamic_state.static_state = static_state;

   sampler->nr_samplers = nr_samplers;
   return &sampler->base;
}


static void
lp_llvm_image_soa_emit_op(const struct lp_build_image_soa *base,
                             struct gallivm_state *gallivm,
                             const struct lp_img_params *params)
{
   struct lp_llvm_image_soa *image = (struct lp_llvm_image_soa *)base;
   const unsigned image_index = params->image_index;
   assert(image_index < PIPE_MAX_SHADER_IMAGES);

   if (params->image_index_offset) {
      struct lp_build_img_op_array_switch switch_info;
      memset(&switch_info, 0, sizeof(switch_info));
      LLVMValueRef unit = LLVMBuildAdd(gallivm->builder,
                                       params->image_index_offset,
                                       lp_build_const_int32(gallivm,
                                                            image_index), "");

      lp_build_image_op_switch_soa(&switch_info, gallivm, params,
                                   unit, 0, image->nr_images);

      for (unsigned i = 0; i < image->nr_images; i++) {
         lp_build_image_op_array_case(&switch_info, i,
                                      &image->dynamic_state.static_state[i].image_state,
                                      &image->dynamic_state.base);
      }
      lp_build_image_op_array_fini_soa(&switch_info);
   } else {
      lp_build_img_op_soa(&image->dynamic_state.static_state[image_index].image_state,
                          &image->dynamic_state.base,
                          gallivm, params, params->outdata);
   }
}


/**
 * Fetch the texture size.
 */
static void
lp_llvm_image_soa_emit_size_query(const struct lp_build_image_soa *base,
                                  struct gallivm_state *gallivm,
                                  const struct lp_sampler_size_query_params *params)
{
   struct lp_llvm_image_soa *image = (struct lp_llvm_image_soa *)base;

   assert(params->texture_unit < PIPE_MAX_SHADER_IMAGES);

   lp_build_size_query_soa(gallivm,
                           &image->dynamic_state.static_state[params->texture_unit].image_state,
                           &image->dynamic_state.base,
                           params);
}


struct lp_build_image_soa *
lp_llvm_image_soa_create(const struct lp_image_static_state *static_state,
                         unsigned nr_images)
{
   struct lp_llvm_image_soa *image = CALLOC_STRUCT(lp_llvm_image_soa);
   if (!image)
      return NULL;

   image->base.emit_op = lp_llvm_image_soa_emit_op;
   image->base.emit_size_query = lp_llvm_image_soa_emit_size_query;

   lp_build_jit_fill_image_dynamic_state(&image->dynamic_state.base);
   image->dynamic_state.static_state = static_state;

   image->nr_images = nr_images;
   return &image->base;
}
