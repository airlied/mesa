/*
 * Copyright © 2016 Red Hat
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

#include "vtn_private.h"
#include "spirv_amd.h"

bool
vtn_handle_amd_gcn_shader_instruction(struct vtn_builder *b, uint32_t ext_opcode,
                                      const uint32_t *w, unsigned count)
{
   nir_intrinsic_op op;
   const struct glsl_type *dest_type =
      vtn_value(b, w[1], vtn_value_type_type)->type->type;

   switch ((enum AMDSPVGCNShader)ext_opcode) {
   case SpvOpCubeFaceIndexAMD:
      op = nir_intrinsic_cube_face_index;
      break;
   case SpvOpCubeFaceCoordAMD:
      op = nir_intrinsic_cube_face_coord;
      break;
   case SpvOpTimeAMD:
      op = nir_intrinsic_time;
      break;
   default:
      unreachable("Invalid opcode");
   }

   struct vtn_value *val = vtn_push_value(b, w[2], vtn_value_type_ssa);
   val->ssa = vtn_create_ssa_value(b, dest_type);

   nir_intrinsic_instr *intrin = nir_intrinsic_instr_create(b->nb.shader, op);

   if (ext_opcode != SpvOpTimeAMD)
      intrin->src[0] = nir_src_for_ssa(vtn_ssa_value(b, w[5])->def);
   nir_ssa_dest_init(&intrin->instr, &intrin->dest, glsl_get_vector_elements(dest_type),
		     glsl_get_bit_size(dest_type), NULL);
   val->ssa->def = &intrin->dest.ssa;
   nir_builder_instr_insert(&b->nb, &intrin->instr);
   return true;
}

bool
vtn_handle_amd_shader_trinary_minmax_instruction(struct vtn_builder *b, uint32_t ext_opcode,
                                                 const uint32_t *w, unsigned count)
{
   struct nir_builder *nb = &b->nb;
   const struct glsl_type *dest_type =
      vtn_value(b, w[1], vtn_value_type_type)->type->type;
   struct vtn_value *val = vtn_push_value(b, w[2], vtn_value_type_ssa);
   val->ssa = vtn_create_ssa_value(b, dest_type);

   unsigned num_inputs = count - 5;
   assert(num_inputs == 3);
   nir_ssa_def *src[3] = { NULL, };
      for (unsigned i = 0; i < num_inputs; i++)
      src[i] = vtn_ssa_value(b, w[i + 5])->def;
      
   switch ((enum AMDSPVTrinaryMinmax)ext_opcode) {
   case SpvOpFMin3AMD:
      val->ssa->def = nir_fmin3(nb, src[0], src[1], src[2]);
      break;
   case SpvOpUMin3AMD:
      val->ssa->def = nir_umin3(nb, src[0], src[1], src[2]);
      break;
   case SpvOpSMin3AMD:
      val->ssa->def = nir_imin3(nb, src[0], src[1], src[2]);
      break;
   case SpvOpFMax3AMD:
      val->ssa->def = nir_fmax3(nb, src[0], src[1], src[2]);
      break;
   case SpvOpUMax3AMD:
      val->ssa->def = nir_umax3(nb, src[0], src[1], src[2]);
      break;
   case SpvOpSMax3AMD:
      val->ssa->def = nir_imax3(nb, src[0], src[1], src[2]);
      break;
   case SpvOpFMid3AMD:
      val->ssa->def = nir_fmed3(nb, src[0], src[1], src[2]);
      break;
   case SpvOpUMid3AMD:
      val->ssa->def = nir_umed3(nb, src[0], src[1], src[2]);
      break;
   case SpvOpSMid3AMD:
      val->ssa->def = nir_imed3(nb, src[0], src[1], src[2]);
      break;
   }

   return true;
}

bool
vtn_handle_arb_shader_ballot_instruction(struct vtn_builder *b, uint32_t ext_opcode,
					 const uint32_t *w, unsigned count)
{
   struct nir_builder *nb = &b->nb;
   nir_intrinsic_op op;
   const struct glsl_type *dest_type =
      vtn_value(b, w[1], vtn_value_type_type)->type->type;
   struct vtn_value *val = vtn_push_value(b, w[2], vtn_value_type_ssa);
   val->ssa = vtn_create_ssa_value(b, dest_type);
   fprintf(stderr, "got ballot opcode %d\n", ext_opcode);

   switch ((enum ARBSPVShaderBallot)ext_opcode) {
   case BallotARB:
      op = nir_intrinsic_ballot;
      break;
   case ReadInvocationARB:
      op = nir_intrinsic_read_invocation;
      break;
   case ReadFirstInvocationARB:
      op = nir_intrinsic_read_first_invocation;
      break;
   };

   nir_intrinsic_instr *intrin = nir_intrinsic_instr_create(b->nb.shader, op);

   nir_ssa_dest_init(&intrin->instr, &intrin->dest, glsl_get_vector_elements(dest_type),
		     glsl_get_bit_size(dest_type), NULL);
   val->ssa->def = &intrin->dest.ssa;
   nir_builder_instr_insert(&b->nb, &intrin->instr);
   return true;
}
