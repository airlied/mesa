/*
 * Copyright © 2017 Valve Corporation
 * Copyright © 2017 Red Hat
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

#include "vtn_private.h"
#include "GLSL.ext.AMD.h"

void
vtn_handle_group(struct vtn_builder *b, SpvOp opcode,
                 const uint32_t *w, unsigned count)
{
   SpvScope scope =
      vtn_value(b, w[3], vtn_value_type_constant)->constant->values[0].u32[0];
   nir_intrinsic_op op;
   switch (opcode) {
   case SpvOpGroupAll:
      switch (scope) {
      case SpvScopeSubgroup:
         op = nir_intrinsic_vote_all;
         break;
      case SpvScopeWorkgroup:
         op = nir_intrinsic_group_all;
         break;
      default:
         unreachable("bad scope");
      }
      break;
   case SpvOpGroupAny:
      switch (scope) {
      case SpvScopeSubgroup:
         op = nir_intrinsic_vote_any;
         break;
      case SpvScopeWorkgroup:
         op = nir_intrinsic_group_any;
         break;
      default:
         unreachable("bad scope");
      }
      break;
   case SpvOpGroupBroadcast:
      switch (scope) {
      case SpvScopeSubgroup:
         op = nir_intrinsic_read_invocation;
         break;
      case SpvScopeWorkgroup:
         op = nir_intrinsic_group_broadcast;
         break;
      default:
         unreachable("bad scope");
      }
      break;

#define OP(spv, nir) \
   case SpvOpGroup##spv##NonUniformAMD: \
      switch (scope) { \
      case SpvScopeSubgroup: \
         switch ((SpvGroupOperation) w[4]) { \
         case SpvGroupOperationReduce: \
            op = nir_intrinsic_subgroup_##nir##_nonuniform; \
            break; \
         case SpvGroupOperationInclusiveScan: \
            op = nir_intrinsic_subgroup_##nir##_inclusive_scan_nonuniform; \
            break; \
         case SpvGroupOperationExclusiveScan: \
            op = nir_intrinsic_subgroup_##nir##_exclusive_scan_nonuniform; \
            break; \
         default: \
            unreachable("unhandled group operation"); \
         } \
         break; \
      case SpvScopeWorkgroup: \
         switch ((SpvGroupOperation) w[4]) { \
         case SpvGroupOperationReduce: \
            op = nir_intrinsic_group_##nir##_nonuniform; \
            break; \
         case SpvGroupOperationInclusiveScan: \
            op = nir_intrinsic_group_##nir##_inclusive_scan_nonuniform; \
            break; \
         case SpvGroupOperationExclusiveScan: \
            op = nir_intrinsic_group_##nir##_exclusive_scan_nonuniform; \
            break; \
         default: \
            unreachable("unhandled group operation"); \
         } \
         break; \
      default: \
         unreachable("bad scope for AMD_shader_ballot"); \
      } \
      break; \
   case SpvOpGroup##spv: \
      switch (scope) { \
      case SpvScopeSubgroup: \
         switch ((SpvGroupOperation) w[4]) { \
         case SpvGroupOperationReduce: \
            op = nir_intrinsic_subgroup_##nir; \
            break; \
         case SpvGroupOperationInclusiveScan: \
            op = nir_intrinsic_subgroup_##nir##_inclusive_scan; \
            break; \
         case SpvGroupOperationExclusiveScan: \
            op = nir_intrinsic_subgroup_##nir##_exclusive_scan; \
            break; \
         default: \
            unreachable("unhandled group operation"); \
         } \
         break; \
      case SpvScopeWorkgroup: \
         switch ((SpvGroupOperation) w[4]) { \
         case SpvGroupOperationReduce: \
            op = nir_intrinsic_group_##nir; \
            break; \
         case SpvGroupOperationInclusiveScan: \
            op = nir_intrinsic_group_##nir##_inclusive_scan; \
            break; \
         case SpvGroupOperationExclusiveScan: \
            op = nir_intrinsic_group_##nir##_exclusive_scan; \
            break; \
         default: \
            unreachable("unhandled group operation"); \
         } \
         break; \
      default: \
         unreachable("bad scope for group reduction"); \
      } \
      break;

      OP(IAdd, iadd)
      OP(FAdd, fadd)
      OP(FMin, fmin)
      OP(UMin, umin)
      OP(SMin, imin)
      OP(FMax, fmax)
      OP(UMax, umax)
      OP(SMax, imax)

   default:
      unreachable("bad opcode for AMD_shader_ballot");
   }

   nir_intrinsic_instr *intrin =
      nir_intrinsic_instr_create(b->shader, op);

   const uint32_t value =
      (opcode == SpvOpGroupAll ||
       opcode == SpvOpGroupAny ||
       opcode == SpvOpGroupBroadcast) ? w[4] : w[5];
   intrin->src[0] = nir_src_for_ssa(vtn_ssa_value(b, value)->def);

   if (opcode == SpvOpGroupBroadcast) {
      nir_ssa_def *id = vtn_ssa_value(b, w[5])->def;
      if (scope == SpvScopeWorkgroup) {
         /* From the SPIR-V 1.2 spec, OpGroupBroadcast:
          *
          *    "LocalId must be an integer datatype. It can be a scalar, or a
          *    vector with 2 components or a vector with 3 components."
          *
          * Pad it with trailing 0's to make it always 3-dimensional, to match
          * the definition of nir_intrinsic_group_broadcast.
          */
         nir_ssa_def *srcs[3];
         for (unsigned i = 0; i < 3; i++) {
            if (i >= id->num_components)
               srcs[i] = nir_imm_int(&b->nb, 0);
            else
               srcs[i] = nir_channel(&b->nb, id, i);
         }
         id = nir_vec(&b->nb, srcs, 3);
      }
      intrin->src[1] = nir_src_for_ssa(id);
   }

   intrin->num_components = intrin->src[0].ssa->num_components;
   nir_ssa_dest_init(&intrin->instr, &intrin->dest,
                     intrin->num_components,
                     intrin->src[0].ssa->bit_size,
                     NULL);
   nir_builder_instr_insert(&b->nb, &intrin->instr);

   nir_ssa_def *result = &intrin->dest.ssa;
   struct vtn_value *val = vtn_push_value(b, w[2], vtn_value_type_ssa);
   const struct glsl_type *result_type =
      vtn_value(b, w[1], vtn_value_type_type)->type->type;
   val->ssa = vtn_create_ssa_value(b, result_type);
   val->ssa->def = result;
}

bool
vtn_handle_amd_ballot_ext(struct vtn_builder *b, uint32_t opcode,
                          const uint32_t *w, unsigned count)
{
   unsigned num_srcs;
   nir_intrinsic_op op;

   switch ((enum ShaderBallotAMD) opcode) {
   case SwizzleInvocationsAMD: {
      op = nir_intrinsic_quad_swizzle_amd;
      num_srcs = 1;
      break;
   }
   case SwizzleInvocationsMaskedAMD: {
      op = nir_intrinsic_masked_swizzle_amd;
      num_srcs = 1;
      break;
   }
   case WriteInvocationAMD:
      op = nir_intrinsic_write_invocation;
      num_srcs = 3;
      break;
   case MbcntAMD:
      op = nir_intrinsic_mbcnt_amd;
      num_srcs = 1;
      break;
   default:
      unreachable("unknown AMD_shader_ballot opcode");
   }

   nir_intrinsic_instr *intrin =
      nir_intrinsic_instr_create(b->shader, op);

   for (unsigned i = 0; i < num_srcs; i++)
      intrin->src[i] = nir_src_for_ssa(vtn_ssa_value(b, w[5 + i])->def);

   switch ((enum ShaderBallotAMD) opcode) {
   case SwizzleInvocationsAMD: {
      nir_constant *offset = vtn_value(b, w[6], vtn_value_type_constant)->constant;
      unsigned subgroup_data = 0;
      for (unsigned i = 0; i < 4; i++)
         subgroup_data |= offset->values[0].u32[i] << (2 * i);
      nir_intrinsic_set_subgroup_data(intrin, subgroup_data);
      break;
   }
   case SwizzleInvocationsMaskedAMD: {
      nir_constant *mask = vtn_value(b, w[6], vtn_value_type_constant)->constant;
      unsigned subgroup_data = 0;
      for (unsigned i = 0; i < 3; i++)
         subgroup_data |= mask->values[0].u32[i] << (5 * i);
      nir_intrinsic_set_subgroup_data(intrin, subgroup_data);
      break;
   }
   default:
      break;
   }

   intrin->num_components = intrin->src[0].ssa->num_components;
   nir_ssa_dest_init(&intrin->instr, &intrin->dest,
                     intrin->num_components,
                     (enum ShaderBallotAMD) opcode == MbcntAMD ? 32 :
                        intrin->src[0].ssa->bit_size,
                     NULL);
   nir_builder_instr_insert(&b->nb, &intrin->instr);

   nir_ssa_def *result = &intrin->dest.ssa;
   struct vtn_value *val = vtn_push_value(b, w[2], vtn_value_type_ssa);
   const struct glsl_type *result_type =
      vtn_value(b, w[1], vtn_value_type_type)->type->type;
   val->ssa = vtn_create_ssa_value(b, result_type);
   val->ssa->def = result;

   return true;
}

bool
vtn_handle_amd_gcn_shader_instruction(struct vtn_builder *b, uint32_t ext_opcode,
                                      const uint32_t *w, unsigned count)
{
   nir_intrinsic_op op;
   const struct glsl_type *dest_type =
      vtn_value(b, w[1], vtn_value_type_type)->type->type;

   switch ((enum GcnShaderAMD)ext_opcode) {
   case CubeFaceIndexAMD:
      op = nir_intrinsic_cube_face_index;
      break;
   case CubeFaceCoordAMD:
      op = nir_intrinsic_cube_face_coord;
      break;
   case TimeAMD:
      op = nir_intrinsic_time;
      break;
   default:
      unreachable("Invalid opcode");
   }

   struct vtn_value *val = vtn_push_value(b, w[2], vtn_value_type_ssa);
   val->ssa = vtn_create_ssa_value(b, dest_type);

   nir_intrinsic_instr *intrin = nir_intrinsic_instr_create(b->nb.shader, op);

   if (ext_opcode != TimeAMD)
      intrin->src[0] = nir_src_for_ssa(vtn_ssa_value(b, w[5])->def);
   nir_ssa_dest_init(&intrin->instr, &intrin->dest, glsl_get_vector_elements(dest_type),
		     glsl_get_bit_size(dest_type), NULL);
   val->ssa->def = &intrin->dest.ssa;
   nir_builder_instr_insert(&b->nb, &intrin->instr);
   return true;
}
