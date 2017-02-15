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
