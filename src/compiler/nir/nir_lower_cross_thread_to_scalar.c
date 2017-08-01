/*
 * Copyright © 2017 Intel Corporation
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

#include "nir.h"
#include "nir_builder.h"

/** @file nir_lower_cross_thread_to_scalar.c
 *
 * Replaces certain cross-thread intrinsics with num_components != 1 with
 * individual per-channel operations. So far, the operations supported are:
 *
 * - read_invocation
 * - read_first_invocation
 */

static void
lower_to_scalar(nir_builder *b, nir_intrinsic_instr *intrin)
{
   b->cursor = nir_before_instr(&intrin->instr);

   nir_ssa_def *reads[4];

   for (unsigned i = 0; i < intrin->num_components; i++) {
      nir_intrinsic_instr *chan_intrin =
         nir_intrinsic_instr_create(b->shader, intrin->intrinsic);
      nir_ssa_dest_init(&chan_intrin->instr, &chan_intrin->dest,
                        1, intrin->dest.ssa.bit_size, NULL);
      chan_intrin->num_components = 1;
      const nir_intrinsic_info *info = &nir_intrinsic_infos[intrin->intrinsic];

      for (unsigned src = 0; src < info->num_srcs; src++) {
         if (info->src_components[src] != 0) {
            nir_src_copy(&chan_intrin->src[src], &intrin->src[src],
                         chan_intrin);
         } else {
            nir_ssa_def *value = nir_ssa_for_src(b, intrin->src[src],
                                                 intrin->num_components);
            chan_intrin->src[src] = nir_src_for_ssa(nir_channel(b, value, i));
         }
      }

      for (unsigned idx = 0; idx < info->num_indices; idx++)
         chan_intrin->const_index[idx] = intrin->const_index[idx];

      nir_builder_instr_insert(b, &chan_intrin->instr);

      reads[i] = &chan_intrin->dest.ssa;
   }

   nir_ssa_def_rewrite_uses(&intrin->dest.ssa,
                            nir_src_for_ssa(nir_vec(b, reads,
                                                    intrin->num_components)));
   nir_instr_remove(&intrin->instr);
}

static bool
nir_lower_cross_thread_to_scalar_impl(nir_function_impl *impl)
{
   bool progress = false;
   nir_builder b;
   nir_builder_init(&b, impl);

   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;

         nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);

         if (intrin->num_components == 1)
            continue;

         switch (intrin->intrinsic) {
         case nir_intrinsic_read_invocation:
         case nir_intrinsic_read_first_invocation:
            lower_to_scalar(&b, intrin);
            progress = true;
            break;
         default:
            break;
         }
      }
   }

   if (progress) {
      nir_metadata_preserve(impl, nir_metadata_block_index |
                                  nir_metadata_dominance);
   }
   return progress;
}

bool
nir_lower_cross_thread_to_scalar(nir_shader *shader)
{
   bool progress = false;

   nir_foreach_function(function, shader) {
      if (function->impl)
         progress |= nir_lower_cross_thread_to_scalar_impl(function->impl);
   }

   return progress;
}
