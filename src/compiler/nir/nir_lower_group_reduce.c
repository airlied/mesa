/*
 * Copyright © 2017 Valve Corporation
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
 *
 */

#include "nir.h"
#include "nir_builder.h"

/*
 * Implement this workgroup operations using operations on shared variables:
 *
 * - group_broadcast
 * - group_any
 * - group_all
 */

static nir_ssa_def *
build_subgroup_any(nir_builder *b, nir_ssa_def *src)
{
   nir_intrinsic_instr *instr = nir_intrinsic_instr_create(b->shader,
                                                           nir_intrinsic_vote_any);
   nir_ssa_dest_init(&instr->instr, &instr->dest, 1, 32, NULL);
   instr->src[0] = nir_src_for_ssa(src);
   nir_builder_instr_insert(b, &instr->instr);
   return &instr->dest.ssa;
}

static void
build_barrier(nir_builder *b)
{
   nir_intrinsic_instr *intrin =
      nir_intrinsic_instr_create(b->shader, nir_intrinsic_barrier);
   nir_builder_instr_insert(b, &intrin->instr);
}

/* TODO share this between different instructions */

static nir_variable *
alloc_shared_temp(nir_shader *shader, unsigned components,
                  unsigned bit_size)
{
   enum glsl_base_type base_type;
   switch (bit_size) {
   case 32:
      base_type = GLSL_TYPE_UINT;
      break;
   case 64:
      base_type = GLSL_TYPE_UINT64;
      break;
   default:
      unreachable("bad bit size");
   }

   const struct glsl_type *type;
   if (components == 1)
      type = glsl_scalar_type(base_type);
   else
      type = glsl_vector_type(base_type, components);
   return nir_variable_create(shader, nir_var_shared, type, "shared_temp");
}

static nir_ssa_def *
build_group_any(nir_builder *b, nir_ssa_def *src)
{
   assert(src->num_components == 1);
   nir_variable *temp = nir_variable_create(b->shader, nir_var_shared,
                                            glsl_bool_type(), "any_temp");

   nir_store_var(b, temp, nir_imm_int(b, NIR_FALSE), 1);
   build_barrier(b);
   nir_push_if(b, build_subgroup_any(b, src));
   nir_store_var(b, temp, nir_imm_int(b, NIR_TRUE), 1);
   nir_pop_if(b, NULL);
   build_barrier(b);
   return nir_load_var(b, temp);
}

static nir_ssa_def *
build_group_all(nir_builder *b, nir_ssa_def *src)
{
   assert(src->num_components == 1);
   nir_variable *temp = nir_variable_create(b->shader, nir_var_shared,
                                            glsl_bool_type(), "all_temp");

   nir_store_var(b, temp, nir_imm_int(b, NIR_TRUE), 1);
   build_barrier(b);
   nir_push_if(b, build_subgroup_any(b, nir_inot(b, src)));
   nir_store_var(b, temp, nir_imm_int(b, NIR_FALSE), 1);
   nir_pop_if(b, NULL);
   build_barrier(b);
   return nir_load_var(b, temp);
}

static nir_ssa_def *
build_group_broadcast(nir_builder *b, nir_ssa_def *src, nir_ssa_def *id)
{
   nir_variable *temp = alloc_shared_temp(b->shader, src->num_components,
                                          src->bit_size);

   nir_push_if(b, nir_ball_iequal3(b, id, nir_load_local_invocation_id(b)));
   nir_store_var(b, temp, src, (1 << src->num_components) - 1);
   nir_pop_if(b, NULL);
   build_barrier(b);
   return nir_load_var(b, temp);
}

static bool
lower_group_reduce_impl(nir_function_impl *impl,
                        const struct shader_info *info)
{
   nir_builder b;
   nir_builder_init(&b, impl);
   bool progress = false;

   nir_foreach_block_safe(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;

         nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
         nir_ssa_def *replacement = NULL;
         b.cursor = nir_before_instr(instr);

         switch (intrin->intrinsic) {
         case nir_intrinsic_group_any:
            replacement = build_group_any(&b, intrin->src[0].ssa);
            break;
         case nir_intrinsic_group_all:
            replacement = build_group_all(&b, intrin->src[0].ssa);
            break;
         case nir_intrinsic_group_broadcast:
            replacement = build_group_broadcast(&b, intrin->src[0].ssa,
                                                intrin->src[1].ssa);
            break;
         default:
            continue;
         }

         assert(replacement);
         nir_ssa_def_rewrite_uses(&intrin->dest.ssa,
                                  nir_src_for_ssa(replacement));
         progress = true;
      }
   }

   return progress;
}

bool
nir_lower_group_reduce(nir_shader *shader)
{
   bool progress = false;

   nir_foreach_function(function, shader) {
      if (function->impl)
         progress |= lower_group_reduce_impl(function->impl, &shader->info);
   }

   return false;
}
