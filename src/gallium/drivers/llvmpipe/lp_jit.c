/**************************************************************************
 *
 * Copyright 2009 VMware, Inc.
 * All Rights Reserved.
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
 * @file
 * C - JIT interfaces
 *
 * @author Jose Fonseca <jfonseca@vmware.com>
 */

#include <llvm/Config/llvm-config.h>

#include "util/u_memory.h"
#include "gallivm/lp_bld_init.h"
#include "gallivm/lp_bld_debug.h"
#include "gallivm/lp_bld_format.h"
#include "lp_context.h"
#include "lp_screen.h"
#include "lp_jit.h"

static void
lp_jit_create_types(struct lp_fragment_shader_variant *lp)
{
   struct gallivm_state *gallivm = lp->gallivm;
   LLVMContextRef lc = gallivm->context;
   LLVMTypeRef viewport_type;
   LLVMTypeRef linear_elem_type;

   /* struct lp_jit_viewport */
   {
      LLVMTypeRef elem_types[LP_JIT_VIEWPORT_NUM_FIELDS];

      elem_types[LP_JIT_VIEWPORT_MIN_DEPTH] =
      elem_types[LP_JIT_VIEWPORT_MAX_DEPTH] = LLVMFloatTypeInContext(lc);

      viewport_type = LLVMStructTypeInContext(lc, elem_types,
                                              ARRAY_SIZE(elem_types), 0);

      LP_CHECK_MEMBER_OFFSET(struct lp_jit_viewport, min_depth,
                             gallivm->target, viewport_type,
                             LP_JIT_VIEWPORT_MIN_DEPTH);
      LP_CHECK_MEMBER_OFFSET(struct lp_jit_viewport, max_depth,
                             gallivm->target, viewport_type,
                             LP_JIT_VIEWPORT_MAX_DEPTH);
      LP_CHECK_STRUCT_SIZE(struct lp_jit_viewport,
                           gallivm->target, viewport_type);
   }


   /* struct lp_jit_context */
   {
      LLVMTypeRef elem_types[LP_JIT_CTX_COUNT];
      LLVMTypeRef context_type;

      elem_types[LP_JIT_CTX_ALPHA_REF] = LLVMFloatTypeInContext(lc);
      elem_types[LP_JIT_CTX_SAMPLE_MASK] =
      elem_types[LP_JIT_CTX_STENCIL_REF_FRONT] =
      elem_types[LP_JIT_CTX_STENCIL_REF_BACK] = LLVMInt32TypeInContext(lc);
      elem_types[LP_JIT_CTX_U8_BLEND_COLOR] = LLVMPointerType(LLVMInt8TypeInContext(lc), 0);
      elem_types[LP_JIT_CTX_F_BLEND_COLOR] = LLVMPointerType(LLVMFloatTypeInContext(lc), 0);
      elem_types[LP_JIT_CTX_VIEWPORTS] = LLVMPointerType(viewport_type, 0);

      context_type = LLVMStructTypeInContext(lc, elem_types,
                                             ARRAY_SIZE(elem_types), 0);

      LP_CHECK_MEMBER_OFFSET(struct lp_jit_context, alpha_ref_value,
                             gallivm->target, context_type,
                             LP_JIT_CTX_ALPHA_REF);
      LP_CHECK_MEMBER_OFFSET(struct lp_jit_context, stencil_ref_front,
                             gallivm->target, context_type,
                             LP_JIT_CTX_STENCIL_REF_FRONT);
      LP_CHECK_MEMBER_OFFSET(struct lp_jit_context, stencil_ref_back,
                             gallivm->target, context_type,
                             LP_JIT_CTX_STENCIL_REF_BACK);
      LP_CHECK_MEMBER_OFFSET(struct lp_jit_context, u8_blend_color,
                             gallivm->target, context_type,
                             LP_JIT_CTX_U8_BLEND_COLOR);
      LP_CHECK_MEMBER_OFFSET(struct lp_jit_context, f_blend_color,
                             gallivm->target, context_type,
                             LP_JIT_CTX_F_BLEND_COLOR);
      LP_CHECK_MEMBER_OFFSET(struct lp_jit_context, viewports,
                             gallivm->target, context_type,
                             LP_JIT_CTX_VIEWPORTS);
      LP_CHECK_MEMBER_OFFSET(struct lp_jit_context, sample_mask,
                             gallivm->target, context_type,
                             LP_JIT_CTX_SAMPLE_MASK);
      LP_CHECK_STRUCT_SIZE(struct lp_jit_context,
                           gallivm->target, context_type);

      lp->jit_context_type = context_type;
      lp->jit_context_ptr_type = LLVMPointerType(context_type, 0);
      lp->jit_resources_type = lp_build_jit_resources_type(gallivm);
      lp->jit_resources_ptr_type = LLVMPointerType(lp->jit_resources_type, 0);
   }

   /* struct lp_jit_thread_data */
   {
      LLVMTypeRef elem_types[LP_JIT_THREAD_DATA_COUNT];
      LLVMTypeRef thread_data_type;

      elem_types[LP_JIT_THREAD_DATA_CACHE] =
            LLVMPointerType(lp_build_format_cache_type(gallivm), 0);
      elem_types[LP_JIT_THREAD_DATA_VIS_COUNTER] = LLVMInt64TypeInContext(lc);
      elem_types[LP_JIT_THREAD_DATA_PS_INVOCATIONS] = LLVMInt64TypeInContext(lc);
      elem_types[LP_JIT_THREAD_DATA_RASTER_STATE_VIEWPORT_INDEX] =
      elem_types[LP_JIT_THREAD_DATA_RASTER_STATE_VIEW_INDEX] =
            LLVMInt32TypeInContext(lc);

      thread_data_type = LLVMStructTypeInContext(lc, elem_types,
                                                 ARRAY_SIZE(elem_types), 0);

      lp->jit_thread_data_type = thread_data_type;
      lp->jit_thread_data_ptr_type = LLVMPointerType(thread_data_type, 0);
   }

   /*
    * lp_linear_elem
    *
    * XXX: it can be instanced only once due to the use of opaque types, and
    * the fact that screen->module is also a global.
    */
   {
      LLVMTypeRef ret_type;
      LLVMTypeRef arg_types[1];
      LLVMTypeRef func_type;

      ret_type = LLVMPointerType(LLVMVectorType(LLVMInt8TypeInContext(lc), 16), 0);

      arg_types[0] = LLVMPointerType(LLVMInt8TypeInContext(lc), 0);

      /* lp_linear_func */
      func_type = LLVMFunctionType(ret_type, arg_types, ARRAY_SIZE(arg_types), 0);

      /*
       * We actually define lp_linear_elem not as a structure but simply as a
       * lp_linear_func pointer
       */
      lp->jit_linear_func_type = func_type;
      linear_elem_type = LLVMPointerType(func_type, 0);
   }

   /* struct lp_jit_linear_context */
   {
      LLVMTypeRef linear_elem_ptr_type = LLVMPointerType(linear_elem_type, 0);
      LLVMTypeRef elem_types[LP_JIT_LINEAR_CTX_COUNT];
      LLVMTypeRef linear_context_type;


      elem_types[LP_JIT_LINEAR_CTX_CONSTANTS] = LLVMPointerType(LLVMInt8TypeInContext(lc), 0);
      elem_types[LP_JIT_LINEAR_CTX_TEX] =
      lp->jit_linear_textures_type =
            LLVMArrayType(linear_elem_ptr_type, LP_MAX_LINEAR_TEXTURES);

      elem_types[LP_JIT_LINEAR_CTX_INPUTS] =
      lp->jit_linear_inputs_type =
            LLVMArrayType(linear_elem_ptr_type, LP_MAX_LINEAR_INPUTS);
      elem_types[LP_JIT_LINEAR_CTX_COLOR0] = LLVMPointerType(LLVMInt8TypeInContext(lc), 0);
      elem_types[LP_JIT_LINEAR_CTX_BLEND_COLOR] = LLVMInt32TypeInContext(lc);
      elem_types[LP_JIT_LINEAR_CTX_ALPHA_REF] = LLVMInt8TypeInContext(lc);

      linear_context_type = LLVMStructTypeInContext(lc, elem_types,
                                                    ARRAY_SIZE(elem_types), 0);

      LP_CHECK_MEMBER_OFFSET(struct lp_jit_linear_context, constants,
                             gallivm->target, linear_context_type,
                             LP_JIT_LINEAR_CTX_CONSTANTS);
      LP_CHECK_MEMBER_OFFSET(struct lp_jit_linear_context, tex,
                             gallivm->target, linear_context_type,
                             LP_JIT_LINEAR_CTX_TEX);
      LP_CHECK_MEMBER_OFFSET(struct lp_jit_linear_context, inputs,
                             gallivm->target, linear_context_type,
                             LP_JIT_LINEAR_CTX_INPUTS);
      LP_CHECK_MEMBER_OFFSET(struct lp_jit_linear_context, color0,
                             gallivm->target, linear_context_type,
                             LP_JIT_LINEAR_CTX_COLOR0);
      LP_CHECK_MEMBER_OFFSET(struct lp_jit_linear_context, blend_color,
                             gallivm->target, linear_context_type,
                             LP_JIT_LINEAR_CTX_BLEND_COLOR);
      LP_CHECK_MEMBER_OFFSET(struct lp_jit_linear_context, alpha_ref_value,
                             gallivm->target, linear_context_type,
                             LP_JIT_LINEAR_CTX_ALPHA_REF);
      LP_CHECK_STRUCT_SIZE(struct lp_jit_linear_context,
                           gallivm->target, linear_context_type);

      lp->jit_linear_context_type = linear_context_type;
      lp->jit_linear_context_ptr_type = LLVMPointerType(linear_context_type, 0);
   }

   if (gallivm_debug & GALLIVM_DEBUG_IR) {
      char *str = LLVMPrintModuleToString(gallivm->module);
      fprintf(stderr, "%s", str);
      LLVMDisposeMessage(str);
   }
}


void
lp_jit_screen_cleanup(struct llvmpipe_screen *screen)
{
   /* nothing */
}


boolean
lp_jit_screen_init(struct llvmpipe_screen *screen)
{
   return lp_build_init();
}


void
lp_jit_init_types(struct lp_fragment_shader_variant *lp)
{
   if (!lp->jit_context_ptr_type)
      lp_jit_create_types(lp);
}

static void
lp_jit_create_cs_types(struct lp_compute_shader_variant *lp)
{
   struct gallivm_state *gallivm = lp->gallivm;
   LLVMContextRef lc = gallivm->context;

   /* struct lp_jit_cs_thread_data */
   {
      LLVMTypeRef elem_types[LP_JIT_CS_THREAD_DATA_COUNT];
      LLVMTypeRef thread_data_type;

      elem_types[LP_JIT_CS_THREAD_DATA_CACHE] =
            LLVMPointerType(lp_build_format_cache_type(gallivm), 0);

      elem_types[LP_JIT_CS_THREAD_DATA_SHARED] = LLVMPointerType(LLVMInt32TypeInContext(lc), 0);
      thread_data_type = LLVMStructTypeInContext(lc, elem_types,
                                                 ARRAY_SIZE(elem_types), 0);

      lp->jit_cs_thread_data_type = thread_data_type;
      lp->jit_cs_thread_data_ptr_type = LLVMPointerType(thread_data_type, 0);
   }

   /* struct lp_jit_cs_context */
   {
      LLVMTypeRef elem_types[LP_JIT_CS_CTX_COUNT];
      LLVMTypeRef cs_context_type;

      elem_types[LP_JIT_CS_CTX_KERNEL_ARGS] = LLVMPointerType(LLVMInt8TypeInContext(lc), 0);
      elem_types[LP_JIT_CS_CTX_SHARED_SIZE] = LLVMInt32TypeInContext(lc);

      cs_context_type = LLVMStructTypeInContext(lc, elem_types,
                                             ARRAY_SIZE(elem_types), 0);

      LP_CHECK_MEMBER_OFFSET(struct lp_jit_cs_context, kernel_args,
                             gallivm->target, cs_context_type,
                             LP_JIT_CS_CTX_KERNEL_ARGS);
      LP_CHECK_MEMBER_OFFSET(struct lp_jit_cs_context, shared_size,
                             gallivm->target, cs_context_type,
                             LP_JIT_CS_CTX_SHARED_SIZE);
      LP_CHECK_STRUCT_SIZE(struct lp_jit_cs_context,
                           gallivm->target, cs_context_type);

      lp->jit_cs_context_type = cs_context_type;
      lp->jit_cs_context_ptr_type = LLVMPointerType(cs_context_type, 0);
      lp->jit_resources_type = lp_build_jit_resources_type(gallivm);
      lp->jit_resources_ptr_type = LLVMPointerType(lp->jit_resources_type, 0);
   }

   if (gallivm_debug & GALLIVM_DEBUG_IR) {
      char *str = LLVMPrintModuleToString(gallivm->module);
      fprintf(stderr, "%s", str);
      LLVMDisposeMessage(str);
   }
}

void
lp_jit_init_cs_types(struct lp_compute_shader_variant *lp)
{
   if (!lp->jit_cs_context_ptr_type)
      lp_jit_create_cs_types(lp);
}
