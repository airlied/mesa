/*
 * Copyright © 2023 Bas Nieuwenhuizen
 * Copyright © 2024 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

#include "util/macros.h"
#include "glsl_types.h"
#include "nak_private.h"
#include "nir_builder.h"

#define NAK_WARP_SIZE 32

static enum nak_cmat_type
get_nak_cmat_type_from_desc(struct glsl_cmat_description matrix_desc)
{
   unsigned cols = matrix_desc.cols;
   unsigned rows = matrix_desc.rows;
   unsigned element_type = matrix_desc.element_type;
   bool is_float =
      element_type == GLSL_TYPE_FLOAT || element_type == GLSL_TYPE_FLOAT16;
   bool is_integer =
      element_type == GLSL_TYPE_INT || element_type == GLSL_TYPE_UINT ||
      element_type == GLSL_TYPE_INT8 || element_type == GLSL_TYPE_UINT8;

   // MxNxK (A/B/C/D)
   if (matrix_desc.use == GLSL_CMAT_USE_A) {
      // MxK (A)
      if (rows == 16 && cols == 8 && is_float)
         return NAK_CMAT_TYPE_M16N8K8;

      // Overlap with NAK_CMAT_TYPE_M16N16K16
      if (rows == 16 && cols == 16 && is_float)
         return NAK_CMAT_TYPE_M16N8K16;

      // Overlap with NAK_CMAT_TYPE_M16N16K32
      if (rows == 16 && cols == 32 && is_integer)
         return NAK_CMAT_TYPE_M16N8K32;
   } else if (matrix_desc.use == GLSL_CMAT_USE_B) {
      // KxN (B)
      if (rows == 8 && cols == 8 && is_float)
         return NAK_CMAT_TYPE_M16N8K8;

      if (rows == 16 && cols == 8 && is_float)
         return NAK_CMAT_TYPE_M16N8K16;

      if (rows == 16 && cols == 16 && is_float)
         return NAK_CMAT_TYPE_M16N16K16;

      if (rows == 32 && cols == 8 && is_integer)
         return NAK_CMAT_TYPE_M16N8K32;

      if (rows == 32 && cols == 16 && is_integer)
         return NAK_CMAT_TYPE_M16N16K32;
   } else if (matrix_desc.use == GLSL_CMAT_USE_ACCUMULATOR) {
      // MxN (C)
      // Overlap with NAK_CMAT_TYPE_M16N8K16
      if (rows == 16 && cols == 8 && is_float)
         return NAK_CMAT_TYPE_M16N8K8;

      if (rows == 16 && cols == 16 && is_float)
         return NAK_CMAT_TYPE_M16N16K16;

      if (rows == 16 && cols == 8 && is_integer)
         return NAK_CMAT_TYPE_M16N8K32;

      if (rows == 16 && cols == 16 && is_integer)
         return NAK_CMAT_TYPE_M16N16K32;
   }

   return NAK_CMAT_TYPE_UNKNOWN;
}

static enum nak_cmat_type
get_nak_cmat_type_for_muladd(struct glsl_cmat_description a_desc,
                             struct glsl_cmat_description b_desc,
                             struct glsl_cmat_description c_desc)
{
   unsigned m = a_desc.rows;
   unsigned k = b_desc.rows;
   unsigned n = c_desc.cols;
   unsigned element_type = a_desc.element_type;
   bool is_float =
      element_type == GLSL_TYPE_FLOAT || element_type == GLSL_TYPE_FLOAT16;
   bool is_integer =
      element_type == GLSL_TYPE_INT || element_type == GLSL_TYPE_UINT ||
      element_type == GLSL_TYPE_INT8 || element_type == GLSL_TYPE_UINT8;

   if (m == 16 && n == 8 && k == 8 && is_float)
      return NAK_CMAT_TYPE_M16N8K8;

   if (m == 16 && n == 8 && k == 16 && is_float)
      return NAK_CMAT_TYPE_M16N8K16;

   if (m == 16 && n == 16 && k == 16 && is_float)
      return NAK_CMAT_TYPE_M16N16K16;

   if (m == 16 && n == 8 && k == 32 && is_integer)
      return NAK_CMAT_TYPE_M16N8K32;

   if (m == 16 && n == 16 && k == 32 && is_integer)
      return NAK_CMAT_TYPE_M16N16K32;

   return NAK_CMAT_TYPE_UNKNOWN;
}

static unsigned
get_cmat_size(struct glsl_cmat_description matrix_desc)
{
   return matrix_desc.cols * matrix_desc.rows;
}

static unsigned
get_cmat_length(struct glsl_cmat_description matrix_desc)
{
   return get_cmat_size(matrix_desc) / NAK_WARP_SIZE;
}

static nir_def *
load_cmat(nir_builder *b, nir_def *src)
{
   nir_deref_instr *deref = nir_instr_as_deref(src->parent_instr);
   struct glsl_cmat_description matrix_desc =
      *glsl_get_cmat_description(deref->type);

   return nir_build_load_deref(
      b, get_cmat_length(matrix_desc),
      glsl_base_type_bit_size(matrix_desc.element_type), src, 0);
}

static const struct glsl_type *
remap_matrix_type(struct hash_table *mapping, const struct glsl_type *orig)
{
   struct hash_entry *entry = _mesa_hash_table_search(mapping, orig);

   if (entry)
      return entry->data;

   const struct glsl_type *new_type = orig;

   if (glsl_type_is_cmat(orig)) {
      struct glsl_cmat_description matrix_desc =
         *glsl_get_cmat_description(orig);

      new_type = glsl_vector_type(matrix_desc.element_type,
                                  get_cmat_length(matrix_desc));
   } else if (glsl_type_is_array(orig)) {
      const struct glsl_type *elem_type = glsl_get_array_element(orig);
      const struct glsl_type *new_elem_type =
         remap_matrix_type(mapping, elem_type);

      if (elem_type != new_elem_type) {
         new_type = glsl_array_type(new_elem_type, glsl_get_length(orig),
                                    glsl_get_explicit_stride(orig));
      }
   } else if (glsl_type_is_struct(orig)) {
      unsigned i;
      for (i = 0; i < orig->length; i++) {
         const struct glsl_type *field_type = glsl_get_struct_field(orig, i);
         const struct glsl_type *new_field_type =
            remap_matrix_type(mapping, field_type);

         if (field_type != new_field_type) {
            break;
         }
      }

      /* If we found a cmat, remap the structure type */
      if (i < orig->length) {
         struct glsl_struct_field *fields =
            malloc(sizeof(struct glsl_struct_field) * orig->length);

         /* Copy everything that didn't change */
         memcpy(fields, orig->fields.structure,
                sizeof(struct glsl_struct_field) * i);

         /* Remap the rest */
         for (; i < orig->length; i++) {
            fields[i] = *glsl_get_struct_field_data(orig, i);
            fields[i].type = remap_matrix_type(mapping, fields[i].type);
         }

         new_type =
            glsl_struct_type(fields, orig->length, glsl_get_type_name(orig),
                             glsl_struct_type_is_packed(orig));

         free(fields);
      }
   }

   _mesa_hash_table_insert(mapping, orig, (void *)new_type);
   return new_type;
}

static void
compute_matrix_16x8x16_target(struct nir_builder *b,
                              struct glsl_cmat_description desc,
                              nir_def *lane_id, unsigned idx, nir_def **col_ptr,
                              nir_def **row_ptr)
{
   nir_def *group_id = nir_udiv_imm(b, lane_id, 4);
   nir_def *thread_id_in_group = nir_imod_imm(b, lane_id, 4);
   nir_def *col;
   nir_def *row;

   if (desc.use != GLSL_CMAT_USE_B) {
      row = group_id;

      if (idx >= 2)
         row = nir_iadd_imm(b, row, 8);

      col = nir_iadd_imm(b, nir_imul_imm(b, thread_id_in_group, 2), idx & 1);
   } else {
      row = nir_iadd_imm(b, nir_imul_imm(b, thread_id_in_group, 2), idx & 1);

      if (idx >= 2)
         row = nir_iadd_imm(b, row, 8);

      col = group_id;
   }

   *col_ptr = col;
   *row_ptr = row;
}

static void
compute_matrix_16x8x32_target(struct nir_builder *b,
                              struct glsl_cmat_description desc,
                              nir_def *lane_id, unsigned idx, nir_def **col_ptr,
                              nir_def **row_ptr)
{
   nir_def *group_id = nir_udiv_imm(b, lane_id, 4);
   nir_def *thread_id_in_group = nir_imod_imm(b, lane_id, 4);
   nir_def *col;
   nir_def *row;

   if (desc.use == GLSL_CMAT_USE_A) {
      row = group_id;

      if ((idx >= 0 && idx < 4) || (idx >= 8 && idx < 12)) {
         row = group_id;
      } else {
         row = nir_iadd_imm(b, group_id, 8);
      }

      col = nir_iadd_imm(b, nir_imul_imm(b, thread_id_in_group, 4), idx & 3);

      if (idx >= 8)
         col = nir_iadd_imm(b, col, 16);
   } else if (desc.use == GLSL_CMAT_USE_B) {
      row = nir_iadd_imm(b, nir_imul_imm(b, thread_id_in_group, 4), idx & 3);

      if (idx >= 4)
         row = nir_iadd_imm(b, row, 16);

      col = group_id;
   } else {
      assert(desc.use == GLSL_CMAT_USE_ACCUMULATOR);
      row = group_id;

      if (idx >= 2)
         row = nir_iadd_imm(b, row, 8);

      col = nir_iadd_imm(b, nir_imul_imm(b, thread_id_in_group, 2), idx & 1);
   }

   *col_ptr = col;
   *row_ptr = row;
}

static void
compute_matrix_16x16x32_target(struct nir_builder *b,
                               struct glsl_cmat_description desc,
                               nir_def *lane_id, unsigned idx,
                               nir_def **col_ptr, nir_def **row_ptr)
{
   nir_def *group_id = nir_udiv_imm(b, lane_id, 4);
   nir_def *thread_id_in_group = nir_imod_imm(b, lane_id, 4);
   nir_def *col;
   nir_def *row;

   if (desc.use == GLSL_CMAT_USE_A) {
      row = group_id;

      if ((idx >= 0 && idx < 4) || (idx >= 8 && idx < 12)) {
         row = group_id;
      } else {
         row = nir_iadd_imm(b, group_id, 8);
      }

      col = nir_iadd_imm(b, nir_imul_imm(b, thread_id_in_group, 4), idx & 3);

      if (idx >= 8)
         col = nir_iadd_imm(b, col, 16);
   } else if (desc.use == GLSL_CMAT_USE_B) {
      if ((idx >= 0 && idx < 4) || (idx >= 8 && idx < 12)) {
         col = group_id;
      } else {
         col = nir_iadd_imm(b, group_id, 8);
      }

      row = nir_iadd_imm(b, nir_imul_imm(b, thread_id_in_group, 4), idx & 3);

      if (idx >= 8)
         row = nir_iadd_imm(b, row, 16);
   } else {
      assert(desc.use == GLSL_CMAT_USE_ACCUMULATOR);
      row = group_id;

      if ((idx % 4) >= 2)
         row = nir_iadd_imm(b, row, 8);

      col = nir_iadd_imm(b, nir_imul_imm(b, thread_id_in_group, 2),
                         (idx & 1) + (idx / 4) * 8);
   }

   *col_ptr = col;
   *row_ptr = row;
}

static void
compute_matrix_offsets(struct nir_builder *b, struct glsl_cmat_description desc,
                       enum glsl_matrix_layout layout, nir_def *lane_id,
                       unsigned idx, nir_def **col_offset, nir_def **row_offset)
{
   enum nak_cmat_type cmat_type = get_nak_cmat_type_from_desc(desc);

   switch (cmat_type) {
   case NAK_CMAT_TYPE_M16N8K8:
   case NAK_CMAT_TYPE_M16N8K16:
   case NAK_CMAT_TYPE_M16N16K16:
      compute_matrix_16x8x16_target(b, desc, lane_id, idx % 4, col_offset,
                                    row_offset);
      *col_offset = nir_iadd_imm(b, *col_offset, (idx / 4) * 8);
      break;
   case NAK_CMAT_TYPE_M16N8K32:
      compute_matrix_16x8x32_target(b, desc, lane_id, idx % 16, col_offset,
                                    row_offset);
      break;

   case NAK_CMAT_TYPE_M16N16K32:
      compute_matrix_16x16x32_target(b, desc, lane_id, idx % 16, col_offset,
                                     row_offset);
      break;

   default:
      unreachable("Unknown cmat_type");
   }

   if (layout == GLSL_MATRIX_LAYOUT_ROW_MAJOR) {
      nir_def *tmp = *col_offset;
      *col_offset = *row_offset;
      *row_offset = tmp;
   }
}

static enum nak_cmat_type
get_hw_nak_cmat_type(enum nak_cmat_type cmat_type, uint8_t sm)
{
   switch (cmat_type) {
   case NAK_CMAT_TYPE_M16N16K16:
      return NAK_CMAT_TYPE_M16N8K16;
   case NAK_CMAT_TYPE_M16N16K32:
   case NAK_CMAT_TYPE_M16N8K32:
      /* Turing only support M16N8K16 */
      return sm >= 80 ? NAK_CMAT_TYPE_M16N8K32 : NAK_CMAT_TYPE_M8N8K16;
   default:
      return cmat_type;
   }
}

static nir_def *
lower_cmat_muladd(nir_builder *b, nir_intrinsic_instr *intr, nir_def *cmat_a,
                  nir_def *cmat_b, nir_def *cmat_c,
                  struct glsl_cmat_description a_desc,
                  struct glsl_cmat_description b_desc,
                  struct glsl_cmat_description c_desc,
                  struct glsl_cmat_description d_desc, uint8_t sm)
{
   unsigned dst_length = get_cmat_length(d_desc);

   nir_def *ret;

   /* MxNxK */
   enum nak_cmat_type cmat_type =
      get_nak_cmat_type_for_muladd(a_desc, b_desc, c_desc);
   enum nak_cmat_type hw_cmat_type = get_hw_nak_cmat_type(cmat_type, sm);

   const struct nak_nir_cmat_mul_add_flags flags = {
      .cmat_type = hw_cmat_type,
      .a_type = a_desc.element_type,
      .b_type = b_desc.element_type,
   };
   uint32_t flags_u32;
   STATIC_ASSERT(sizeof(flags_u32) == sizeof(flags));
   memcpy(&flags_u32, &flags, sizeof(flags_u32));

   if (cmat_type != hw_cmat_type) {
      unsigned a_length = get_cmat_length(a_desc);
      unsigned b_length = get_cmat_length(b_desc);
      unsigned c_length = get_cmat_length(c_desc);

      nir_def *a_comps[NIR_MAX_VEC_COMPONENTS];
      nir_def *b_comps[NIR_MAX_VEC_COMPONENTS];
      nir_def *c_comps[NIR_MAX_VEC_COMPONENTS];
      nir_def *d_comps[NIR_MAX_VEC_COMPONENTS];

      for (unsigned i = 0; i < a_length; i++)
         a_comps[i] = nir_channel(b, cmat_a, i);

      for (unsigned i = 0; i < b_length; i++)
         b_comps[i] = nir_channel(b, cmat_b, i);

      for (unsigned i = 0; i < c_length; i++)
         c_comps[i] = nir_channel(b, cmat_c, i);

      if (cmat_type == NAK_CMAT_TYPE_M16N16K16) {
         nir_def *cmat_b_low = nir_vec(b, b_comps, b_length / 2);
         nir_def *cmat_b_high =
            nir_vec(b, &b_comps[b_length / 2], b_length / 2);

         nir_def *cmat_c_low = nir_vec(b, c_comps, c_length / 2);
         nir_def *cmat_c_high =
            nir_vec(b, &c_comps[c_length / 2], c_length / 2);

         nir_def *cmat_d_low =
            nir_cmat_muladd_nv(b, dst_length / 2, cmat_a, cmat_b_low,
                               cmat_c_low, .flags = flags_u32);
         nir_def *cmat_d_high =
            nir_cmat_muladd_nv(b, dst_length / 2, cmat_a, cmat_b_high,
                               cmat_c_high, .flags = flags_u32);

         for (unsigned i = 0; i < dst_length / 2; i++)
            d_comps[i] = nir_channel(b, cmat_d_low, i);
         for (unsigned i = 0; i < dst_length / 2; i++)
            d_comps[dst_length / 2 + i] = nir_channel(b, cmat_d_high, i);
      } else if (hw_cmat_type == NAK_CMAT_TYPE_M8N8K16 &&
                 (cmat_type == NAK_CMAT_TYPE_M16N8K32 ||
                  cmat_type == NAK_CMAT_TYPE_M16N16K32)) {
         const unsigned a_hw_length = 4;
         const unsigned b_hw_length = 4;
         const unsigned c_hw_length = 2;
         const unsigned d_hw_length = 2;

         for (unsigned i = 0; i < dst_length / d_hw_length; i++) {
            unsigned cmat_a_low_offset = (i % 2) * a_hw_length;
            unsigned cmat_a_high_offset = cmat_a_low_offset + 8;
            unsigned cmat_b_low_offset = (i / 2) * b_hw_length;
            unsigned cmat_b_high_offset = cmat_b_low_offset + 4;
            unsigned cmat_c_offset = i * c_hw_length;

            if (cmat_type == NAK_CMAT_TYPE_M16N16K32)
               cmat_b_high_offset = cmat_b_low_offset + 8;

            nir_def *cmat_a_low =
               nir_vec(b, &a_comps[cmat_a_low_offset], a_hw_length);
            nir_def *cmat_a_high =
               nir_vec(b, &a_comps[cmat_a_high_offset], a_hw_length);
            nir_def *cmat_b_low =
               nir_vec(b, &b_comps[cmat_b_low_offset], b_hw_length);
            nir_def *cmat_b_high =
               nir_vec(b, &b_comps[cmat_b_high_offset], b_hw_length);
            nir_def *c_part = nir_vec(b, &c_comps[cmat_c_offset], c_hw_length);

            nir_def *new_c =
               nir_cmat_muladd_nv(b, d_hw_length, cmat_a_low, cmat_b_low,
                                  c_part, .flags = flags_u32);
            nir_def *tmp_d =
               nir_cmat_muladd_nv(b, d_hw_length, cmat_a_high, cmat_b_high,
                                  new_c, .flags = flags_u32);

            for (unsigned c = 0; c < d_hw_length; c++)
               d_comps[i * d_hw_length + c] = nir_channel(b, tmp_d, c);
         }
      } else if (cmat_type == NAK_CMAT_TYPE_M16N16K32) {
         nir_def *b_low_comps[NIR_MAX_VEC_COMPONENTS];
         nir_def *b_high_comps[NIR_MAX_VEC_COMPONENTS];

         assert(b_length == 16);

         for (unsigned i = 0; i < 4; i++) {
            b_low_comps[i] = nir_channel(b, cmat_b, i);
            b_low_comps[i + 4] = nir_channel(b, cmat_b, i + 8);
            b_high_comps[i] = nir_channel(b, cmat_b, i + 4);
            b_high_comps[i + 4] = nir_channel(b, cmat_b, i + 12);
         }

         nir_def *cmat_b_low = nir_vec(b, b_low_comps, b_length / 2);
         nir_def *cmat_b_high = nir_vec(b, b_high_comps, b_length / 2);

         nir_def *cmat_c_low = nir_vec(b, c_comps, c_length / 2);
         nir_def *cmat_c_high =
            nir_vec(b, &c_comps[c_length / 2], c_length / 2);

         nir_def *cmat_d_low =
            nir_cmat_muladd_nv(b, dst_length / 2, cmat_a, cmat_b_low,
                               cmat_c_low, .flags = flags_u32);
         nir_def *cmat_d_high =
            nir_cmat_muladd_nv(b, dst_length / 2, cmat_a, cmat_b_high,
                               cmat_c_high, .flags = flags_u32);

         for (unsigned i = 0; i < dst_length / 2; i++)
            d_comps[i] = nir_channel(b, cmat_d_low, i);
         for (unsigned i = 0; i < dst_length / 2; i++)
            d_comps[dst_length / 2 + i] = nir_channel(b, cmat_d_high, i);
      } else {
         assert(0 && "lowering not implemented");
      }

      ret = nir_vec(b, d_comps, dst_length);
   } else {
      ret = nir_cmat_muladd_nv(b, dst_length, cmat_a, cmat_b, cmat_c,
                               .flags = flags_u32);
   }

   return ret;
}

static bool
nak_nir_lower_cooperative_matrix_impl(struct hash_table *type_mapping,
                                      nir_function_impl *impl,
                                      const struct nak_compiler *nak)
{
   bool progress = false;

   /* Remap all cmat temp var to array of scalars */
   nir_foreach_function_temp_variable(var, impl) {
      const struct glsl_type *new_type =
         remap_matrix_type(type_mapping, var->type);
      if (new_type != var->type) {
         var->type = new_type;
         progress = true;
      }
   }

   nir_builder b = nir_builder_create(impl);
   nir_foreach_block_reverse_safe(block, impl) {
      nir_foreach_instr_reverse_safe(instr, block) {
         b.cursor = nir_before_instr(instr);

         /* Remap deref types */
         if (instr->type == nir_instr_type_deref) {
            nir_deref_instr *deref = nir_instr_as_deref(instr);
            const struct glsl_type *new_type =
               remap_matrix_type(type_mapping, deref->type);

            if (new_type != deref->type) {
               deref->type = new_type;
               progress = true;
            }

            continue;
         } else if (instr->type != nir_instr_type_intrinsic) {
            continue;
         }

         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);

         switch (intr->intrinsic) {
         case nir_intrinsic_cmat_construct: {
            nir_deref_instr *dst_deref =
               nir_instr_as_deref(intr->src[0].ssa->parent_instr);
            struct glsl_cmat_description matrix_desc =
               *glsl_get_cmat_description(dst_deref->type);
            nir_def *data = intr->src[1].ssa;

            nir_def *r = nir_replicate(&b, data, get_cmat_length(matrix_desc));

            nir_store_deref(&b, dst_deref, r,
                            nir_component_mask(r->num_components));
            nir_instr_remove(instr);
            progress = true;
            break;
         }
         case nir_intrinsic_cmat_load: {
            nir_deref_instr *dst_deref =
               nir_instr_as_deref(intr->src[0].ssa->parent_instr);
            struct glsl_cmat_description desc =
               *glsl_get_cmat_description(dst_deref->type);
            unsigned length = get_cmat_length(desc);
            enum glsl_matrix_layout layout = nir_intrinsic_matrix_layout(intr);

            nir_deref_instr *deref =
               nir_instr_as_deref(intr->src[1].ssa->parent_instr);
            nir_def *stride = intr->src[2].ssa;

            nir_def *vars[NIR_MAX_VEC_COMPONENTS];
            for (unsigned i = 0; i < length; ++i)
               vars[i] =
                  nir_undef(&b, 1, glsl_base_type_bit_size(desc.element_type));

            nir_def *lane_id = nir_load_subgroup_invocation(&b);

            for (unsigned idx = 0; idx < length; idx++) {
               nir_def *col_offset;
               nir_def *row_offset;

               compute_matrix_offsets(&b, desc, layout, lane_id, idx,
                                      &col_offset, &row_offset);

               col_offset = nir_imul(&b, col_offset, stride);

               col_offset = nir_u2uN(&b, col_offset, deref->def.bit_size);
               row_offset = nir_u2uN(&b, row_offset, deref->def.bit_size);

               nir_deref_instr *iter_deref =
                  nir_build_deref_ptr_as_array(&b, deref, col_offset);
               iter_deref = nir_build_deref_cast(
                  &b, &iter_deref->def, deref->modes,
                  glsl_scalar_type(desc.element_type),
                  glsl_base_type_bit_size(desc.element_type) / 8);
               iter_deref =
                  nir_build_deref_ptr_as_array(&b, iter_deref, row_offset);

               vars[idx] = nir_load_deref(&b, iter_deref);
            }

            nir_def *mat = nir_vec(&b, vars, length);
            nir_store_deref(&b, dst_deref, mat,
                            nir_component_mask(mat->num_components));
            nir_instr_remove(instr);
            progress = true;
            break;
         }
         case nir_intrinsic_cmat_store: {
            enum glsl_matrix_layout layout = nir_intrinsic_matrix_layout(intr);

            nir_deref_instr *deref =
               nir_instr_as_deref(intr->src[0].ssa->parent_instr);
            nir_def *src = intr->src[1].ssa;
            nir_def *stride = intr->src[2].ssa;

            nir_deref_instr *src_deref = nir_instr_as_deref(src->parent_instr);
            struct glsl_cmat_description desc =
               *glsl_get_cmat_description(src_deref->type);
            unsigned length = get_cmat_length(desc);
            src = load_cmat(&b, src);

            nir_def *vars[NIR_MAX_VEC_COMPONENTS];
            for (unsigned i = 0; i < length; i++)
               vars[i] = nir_channel(&b, src, i);

            nir_def *lane_id = nir_load_subgroup_invocation(&b);

            for (unsigned idx = 0; idx < length; idx++) {
               nir_def *col_offset;
               nir_def *row_offset;

               compute_matrix_offsets(&b, desc, layout, lane_id, idx,
                                      &col_offset, &row_offset);

               col_offset = nir_imul(&b, col_offset, stride);

               col_offset = nir_u2uN(&b, col_offset, deref->def.bit_size);
               row_offset = nir_u2uN(&b, row_offset, deref->def.bit_size);

               nir_deref_instr *iter_deref =
                  nir_build_deref_ptr_as_array(&b, deref, col_offset);
               iter_deref = nir_build_deref_cast(
                  &b, &iter_deref->def, deref->modes,
                  glsl_scalar_type(desc.element_type),
                  glsl_base_type_bit_size(desc.element_type) / 8);
               iter_deref =
                  nir_build_deref_ptr_as_array(&b, iter_deref, row_offset);

               nir_store_deref(&b, iter_deref, vars[idx], 1);
            }

            nir_instr_remove(instr);
            progress = true;
            break;
         }
         case nir_intrinsic_cmat_length: {
            struct glsl_cmat_description matrix_desc =
               nir_intrinsic_cmat_desc(intr);
            nir_def_rewrite_uses(&intr->def,
                                 nir_imm_int(&b, get_cmat_length(matrix_desc)));
            nir_instr_remove(instr);
            progress = true;
            break;
         }
         case nir_intrinsic_cmat_muladd: {
            nir_deref_instr *dst_deref =
               nir_instr_as_deref(intr->src[0].ssa->parent_instr);
            struct glsl_cmat_description d_desc =
               *glsl_get_cmat_description(dst_deref->type);
            struct glsl_cmat_description a_desc = *glsl_get_cmat_description(
               nir_instr_as_deref(intr->src[1].ssa->parent_instr)->type);
            struct glsl_cmat_description b_desc = *glsl_get_cmat_description(
               nir_instr_as_deref(intr->src[2].ssa->parent_instr)->type);
            struct glsl_cmat_description c_desc = *glsl_get_cmat_description(
               nir_instr_as_deref(intr->src[3].ssa->parent_instr)->type);
            nir_def *cmat_a = load_cmat(&b, intr->src[1].ssa);
            nir_def *cmat_b = load_cmat(&b, intr->src[2].ssa);
            nir_def *cmat_c = load_cmat(&b, intr->src[3].ssa);

            nir_def *ret =
               lower_cmat_muladd(&b, intr, cmat_a, cmat_b, cmat_c, a_desc,
                                 b_desc, c_desc, d_desc, nak->sm);
            nir_store_deref(&b, dst_deref, ret,
                            nir_component_mask(ret->num_components));
            nir_instr_remove(&intr->instr);
            progress = true;
            break;
         }
         case nir_intrinsic_cmat_unary_op: {
            nir_def *src = load_cmat(&b, intr->src[1].ssa);
            nir_def *ret = nir_build_alu1(&b, nir_intrinsic_alu_op(intr), src);

            nir_store_deref(&b,
                            nir_instr_as_deref(intr->src[0].ssa->parent_instr),
                            ret, nir_component_mask(ret->num_components));
            nir_instr_remove(instr);
            progress = true;
            break;
         }
         case nir_intrinsic_cmat_binary_op: {
            nir_deref_instr *dst_deref =
               nir_instr_as_deref(intr->src[0].ssa->parent_instr);
            nir_def *src_a = load_cmat(&b, intr->src[1].ssa);
            nir_def *src_b = load_cmat(&b, intr->src[2].ssa);
            nir_op op = nir_intrinsic_alu_op(intr);

            nir_def *ret = nir_build_alu2(&b, op, src_a, src_b);
            nir_store_deref(&b, dst_deref, ret,
                            nir_component_mask(ret->num_components));
            nir_instr_remove(instr);
            progress = true;
            break;
         }
         case nir_intrinsic_cmat_scalar_op: {
            nir_deref_instr *dst_deref =
               nir_instr_as_deref(intr->src[0].ssa->parent_instr);
            nir_def *src_a = load_cmat(&b, intr->src[1].ssa);
            nir_op op = nir_intrinsic_alu_op(intr);
            nir_def *ret = nir_build_alu2(&b, op, src_a, intr->src[2].ssa);
            nir_store_deref(&b, dst_deref, ret,
                            nir_component_mask(ret->num_components));
            nir_instr_remove(instr);
            progress = true;
            break;
         }
         case nir_intrinsic_cmat_bitcast: {
            nir_def *mat = load_cmat(&b, intr->src[1].ssa);
            nir_store_deref(&b,
                            nir_instr_as_deref(intr->src[0].ssa->parent_instr),
                            mat, nir_component_mask(mat->num_components));
            nir_instr_remove(instr);
            progress = true;
            break;
         }
         case nir_intrinsic_cmat_extract: {
            nir_def *mat = load_cmat(&b, intr->src[0].ssa);
            nir_def *index = intr->src[1].ssa;
            nir_def *elem = nir_vector_extract(&b, mat, index);
            nir_def_rewrite_uses(&intr->def, elem);
            nir_instr_remove(instr);
            progress = true;
            break;
         }
         case nir_intrinsic_cmat_insert: {
            nir_deref_instr *dst_deref =
               nir_instr_as_deref(intr->src[0].ssa->parent_instr);
            nir_def *elem = intr->src[1].ssa;
            nir_def *mat = load_cmat(&b, intr->src[2].ssa);
            nir_def *index = intr->src[3].ssa;

            nir_def *r = nir_vector_insert(&b, mat, elem, index);
            nir_store_deref(&b, dst_deref, r,
                            nir_component_mask(r->num_components));
            nir_instr_remove(instr);
            progress = true;
            break;
         }
         case nir_intrinsic_cmat_copy: {
            nir_build_copy_deref(&b, intr->src[0].ssa, intr->src[1].ssa);
            nir_instr_remove(instr);
            progress = true;
            break;
         }
         default:
            break;
         }
      }
   }

   return progress;
}

bool
nak_nir_lower_cooperative_matrix(nir_shader *nir,
                                 const struct nak_compiler *nak)
{
   bool progress = false;

   if (nir->info.stage != MESA_SHADER_COMPUTE ||
       !nir->info.cs.has_cooperative_matrix)
      return false;

   struct hash_table *type_mapping = _mesa_pointer_hash_table_create(NULL);

   /* Remap all cmat shader temp var to array of scalars */
   nir_foreach_variable_with_modes(var, nir, nir_var_shader_temp) {
      const struct glsl_type *new_type =
         remap_matrix_type(type_mapping, var->type);

      if (new_type != var->type) {
         var->type = new_type;
         progress = true;
      }
   }

   nir_foreach_function_impl(impl, nir)
      progress |=
         nak_nir_lower_cooperative_matrix_impl(type_mapping, impl, nak);

   nir_function_impl *impl = nir_shader_get_entrypoint(nir);

   return nir_progress(progress, impl, nir_metadata_all);
}
