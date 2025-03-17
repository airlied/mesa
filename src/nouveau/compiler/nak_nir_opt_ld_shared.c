/*
 * Copyright © 2025 Red Hat.
 * SPDX-License-Identifier: MIT
 */
/*
 * optimise LDSM addressing, so large offsets get put into the LDSM instruction.
 */
#include "nir_builder.h"
#include "nak_private.h"

static bool
nak_nir_opt_ld_shared_impl(nir_function_impl *impl)
{
   bool progress = false;
   nir_builder b = nir_builder_create(impl);
   nir_foreach_block_reverse_safe(block, impl) {
      nir_foreach_instr_reverse_safe(instr, block) {
         b.cursor = nir_before_instr(instr);

         if (instr->type != nir_instr_type_intrinsic)
            continue;

         nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
         if (intrin->intrinsic != nir_intrinsic_cmat_load_shared_nv)
            continue;

         nir_alu_instr *alu = nir_instr_as_alu(intrin->src[0].ssa->parent_instr);
         if (!alu)
            continue;

         if (alu->op != nir_op_iadd)
            continue;

         int constidx = -1;
         int ncidx = -1;
         if (nir_src_is_const(alu->src[0].src)) {
             constidx = 0;
             ncidx = 1;
         }
         if (constidx == -1) {
            if (nir_src_is_const(alu->src[1].src)) {
               constidx = 1;
               ncidx = 0;
            }
         }

         if (constidx == -1)
            continue;

         /*
          * rewrite
          * r10 = iadd CONST, r15
          * r20 = cmat_load_shared_nv(r10, r6)
          * to
          * r10 = iadd(r6, r15)
          * r20 = cmat_load_shared_nv(r10, CONST)
          */
         nir_def *new_add = nir_iadd(&b, intrin->src[1].ssa, alu->src[ncidx].src.ssa);
         nir_src_rewrite(&intrin->src[0], new_add);
         nir_src_rewrite(&intrin->src[1], alu->src[constidx].src.ssa);
         progress = true;
      }
   }
   return progress;
}

bool
nak_nir_opt_ld_shared(nir_shader *nir)
{
   bool progress = false;
   if (nir->info.stage != MESA_SHADER_COMPUTE ||
       !nir->info.cs.has_cooperative_matrix)
      return false;


   nir_foreach_function_impl(impl, nir)
      progress |= nak_nir_opt_ld_shared_impl(impl);
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);

   return nir_progress(progress, impl, nir_metadata_control_flow);
}
