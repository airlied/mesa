#include "nvk_descriptor_set_layout.h"
#include "nvk_nir.h"
#include "nvk_pipeline_layout.h"

#include "nir_builder.h"

struct lower_descriptors_ctx {
  const struct nvk_pipeline_layout *layout;
  bool clamp_desc_array_bounds;
  nir_address_format desc_addr_format;
  nir_address_format ubo_addr_format;
  nir_address_format ssbo_addr_format;
  uint32_t set_base_offset;
};

static bool
lower_load_vulkan_descriptor(nir_builder *b, nir_intrinsic_instr *intrin,
                             const struct lower_descriptors_ctx *ctx) {
  b->cursor = nir_before_instr(&intrin->instr);

  nir_ssa_def *index = nir_imm_int(b, 0);

  nir_intrinsic_instr *parent = nir_src_as_intrinsic(intrin->src[0]);
  while (parent->intrinsic == nir_intrinsic_vulkan_resource_reindex) {
    index = nir_iadd(b, index, nir_ssa_for_src(b, intrin->src[1], 1));
    parent = nir_src_as_intrinsic(intrin->src[0]);
  }

  assert(parent->intrinsic == nir_intrinsic_vulkan_resource_index);
  uint32_t set = nir_intrinsic_desc_set(parent);
  uint32_t binding = nir_intrinsic_binding(parent);
  index = nir_iadd(b, index, nir_ssa_for_src(b, parent->src[0], 1));

  const struct nvk_descriptor_set_binding_layout *binding_layout =
      &ctx->layout->set[set].layout->binding[binding];

  if (ctx->clamp_desc_array_bounds)
    index = nir_umin(b, index, nir_imm_int(b, binding_layout->array_size - 1));

  assert(binding_layout->stride > 0);
  nir_ssa_def *desc_ubo_offset = nir_iadd_imm(
      b, nir_imul_imm(b, index, binding_layout->stride), binding_layout->offset);

  unsigned desc_align = (1 << (ffs(binding_layout->stride) - 1));
  desc_align = MIN2(desc_align, 16);

  nir_ssa_def *ubo0 =
     nir_load_ubo(b, 1, 64, nir_imm_int(b, 0), nir_imm_int(b, (ctx->set_base_offset + set * 2) * 4),
                   .align_mul = 8, .align_offset = 0, .range = ~0);

  nir_ssa_def *desc =
     nir_load_global_constant_offset(b, 4, 32, ubo0, desc_ubo_offset,
                     .align_mul = 16, .align_offset = 0);

  nir_ssa_def_rewrite_uses(&intrin->dest.ssa, desc);

  return true;
}

static bool
lower_load_ssbo(nir_builder *b, nir_intrinsic_instr *intrin,
                const struct lower_descriptors_ctx *ctx) {
   b->cursor = nir_before_instr(&intrin->instr);

   nir_ssa_def *addr = nir_channels(b, intrin->src[1].ssa, 0x3);
   addr = nir_pack_bits(b, addr, 64);
   nir_ssa_def *val = nir_load_global(b, addr, 4, nir_dest_num_components(intrin->dest), nir_dest_bit_size(intrin->dest));

   nir_ssa_def_rewrite_uses(&intrin->dest.ssa, val);
   return true;
}

static bool
lower_load_global_constant_offset(nir_builder *b, nir_intrinsic_instr *intrin,
                                  const struct lower_descriptors_ctx *ctx) {
   b->cursor = nir_before_instr(&intrin->instr);

   nir_ssa_def *addr = nir_iadd(b, intrin->src[0].ssa, nir_u2u64(b, intrin->src[1].ssa));
   nir_ssa_def *val = nir_load_global(b, addr, 4, nir_dest_num_components(intrin->dest), nir_dest_bit_size(intrin->dest));

   nir_ssa_def_rewrite_uses(&intrin->dest.ssa, val);
   return true;
}

static bool
lower_store_ssbo(nir_builder *b, nir_intrinsic_instr *intrin,
                const struct lower_descriptors_ctx *ctx) {
   b->cursor = nir_before_instr(&intrin->instr);
   nir_ssa_def *pkd = nir_vec2(b, intrin->src[1].ssa, intrin->src[2].ssa);
   pkd = nir_pack_bits(b, pkd, 64);
//   nir_ssa_def *addr = nir_iadd(b, pkd, nir_u2u64(b, intrin->src[2].ssa));
   nir_ssa_def *addr = pkd;
   nir_store_global(b, addr, 4, intrin->src[0].ssa, 0x1);

   nir_instr_remove(&intrin->instr);
   return true;
}

static void get_resource_deref_binding(nir_builder *b, nir_deref_instr *deref,
                                       uint32_t *set, uint32_t *binding,
                                       nir_ssa_def **index) {
  if (deref->deref_type == nir_deref_type_array) {
    assert(deref->arr.index.is_ssa);
    *index = deref->arr.index.ssa;
    deref = nir_deref_instr_parent(deref);
  } else {
    *index = nir_imm_int(b, 0);
  }

  assert(deref->deref_type == nir_deref_type_var);
  nir_variable *var = deref->var;

  *set = var->data.descriptor_set;
  *binding = var->data.binding;
}

static nir_ssa_def *
load_resource_deref_desc(nir_builder *b, nir_deref_instr *deref,
                         unsigned desc_offset, unsigned num_components,
                         unsigned bit_size,
                         const struct lower_descriptors_ctx *ctx) {
  uint32_t set, binding;
  nir_ssa_def *index;
  get_resource_deref_binding(b, deref, &set, &binding, &index);

  const struct nvk_descriptor_set_binding_layout *binding_layout =
      &ctx->layout->set[set].layout->binding[binding];

  if (ctx->clamp_desc_array_bounds)
    index = nir_umin(b, index, nir_imm_int(b, binding_layout->array_size - 1));

  const uint32_t desc_ubo_index = set; /* TODO */

  assert(binding_layout->stride > 0);
  nir_ssa_def *desc_ubo_offset =
      nir_iadd_imm(b, nir_imul_imm(b, index, binding_layout->stride),
                   binding_layout->offset + desc_offset);

  unsigned desc_align = (1 << (ffs(binding_layout->stride) - 1));
  desc_align = MIN2(desc_align, 16);

  return nir_load_ubo(b, num_components, bit_size,
                      nir_imm_int(b, desc_ubo_index), desc_ubo_offset,
                      .align_mul = desc_align,
                      .align_offset = (desc_offset % desc_align), .range = ~0);
}

static bool lower_image_intrin(nir_builder *b, nir_intrinsic_instr *intrin,
                               const struct lower_descriptors_ctx *ctx) {
  nir_deref_instr *deref = nir_src_as_deref(intrin->src[0]);
  nir_ssa_def *desc = load_resource_deref_desc(b, deref, 0, 1, 32, ctx);
  nir_rewrite_image_intrinsic(intrin, desc, true);
  return true;
}

static bool lower_intrin(nir_builder *b, nir_intrinsic_instr *intrin,
                         const struct lower_descriptors_ctx *ctx) {
  switch (intrin->intrinsic) {
  case nir_intrinsic_load_vulkan_descriptor:
    return lower_load_vulkan_descriptor(b, intrin, ctx);

  case nir_intrinsic_load_global_constant_offset:
     return lower_load_global_constant_offset(b, intrin, ctx);

  case nir_intrinsic_load_ssbo:
     return lower_load_ssbo(b, intrin, ctx);

  case nir_intrinsic_store_ssbo:
     return lower_store_ssbo(b, intrin, ctx);

  case nir_intrinsic_image_deref_load:
  case nir_intrinsic_image_deref_store:
  case nir_intrinsic_image_deref_atomic_add:
  case nir_intrinsic_image_deref_atomic_imin:
  case nir_intrinsic_image_deref_atomic_umin:
  case nir_intrinsic_image_deref_atomic_imax:
  case nir_intrinsic_image_deref_atomic_umax:
  case nir_intrinsic_image_deref_atomic_and:
  case nir_intrinsic_image_deref_atomic_or:
  case nir_intrinsic_image_deref_atomic_xor:
  case nir_intrinsic_image_deref_atomic_exchange:
  case nir_intrinsic_image_deref_atomic_comp_swap:
  case nir_intrinsic_image_deref_atomic_fadd:
  case nir_intrinsic_image_deref_size:
  case nir_intrinsic_image_deref_samples:
  case nir_intrinsic_image_deref_load_param_intel:
  case nir_intrinsic_image_deref_load_raw_intel:
  case nir_intrinsic_image_deref_store_raw_intel:
    return lower_image_intrin(b, intrin, ctx);

  default:
    return false;
  }
}

static bool lower_tex(nir_builder *b, nir_tex_instr *tex,
                      const struct lower_descriptors_ctx *ctx) {
  b->cursor = nir_before_instr(&tex->instr);

  for (unsigned i = 0; i < tex->num_srcs; i++) {
    if (tex->src[i].src_type != nir_tex_src_texture_deref &&
        tex->src[i].src_type != nir_tex_src_sampler_deref)
      continue;

    nir_deref_instr *deref = nir_src_as_deref(tex->src[i].src);
    nir_ssa_def *desc = load_resource_deref_desc(b, deref, 0, 1, 32, ctx);

    switch (tex->src[i].src_type) {
    case nir_tex_src_texture_deref:
      tex->src[i].src_type = nir_tex_src_texture_handle;
      nir_instr_rewrite_src_ssa(&tex->instr, &tex->src[i].src,
                                nir_iand_imm(b, desc, 0x000fffff));
      break;

    case nir_tex_src_sampler_deref:
      tex->src[i].src_type = nir_tex_src_sampler_handle;
      nir_instr_rewrite_src_ssa(&tex->instr, &tex->src[i].src,
                                nir_iand_imm(b, desc, 0xfff00000));
      break;

    default:
      unreachable("Unhandled texture source");
    }
  }

  return true;
}

static bool lower_descriptors_instr(nir_builder *b, nir_instr *instr,
                                    void *_data) {
  const struct lower_descriptors_ctx *ctx = _data;

  switch (instr->type) {
  case nir_instr_type_tex:
    return lower_tex(b, nir_instr_as_tex(instr), ctx);
  case nir_instr_type_intrinsic:
    return lower_intrin(b, nir_instr_as_intrinsic(instr), ctx);
  default:
    return false;
  }
}

bool nvk_nir_lower_descriptors(nir_shader *nir,
                               const struct nvk_pipeline_layout *layout,
                               bool robust_buffer_access) {
  struct lower_descriptors_ctx ctx = {
      .layout = layout,
      .clamp_desc_array_bounds = robust_buffer_access,
      .desc_addr_format = nir_address_format_32bit_index_offset,
      .ubo_addr_format = nir_address_format_32bit_index_offset,
      .ssbo_addr_format = robust_buffer_access
                              ? nir_address_format_64bit_bounded_global
                              : nir_address_format_64bit_global_32bit_offset,
      .set_base_offset = 8,
  };
  return nir_shader_instructions_pass(
      nir, lower_descriptors_instr,
      nir_metadata_block_index | nir_metadata_dominance, (void *)&ctx);
}
