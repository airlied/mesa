/**************************************************************************
 * 
 * Copyright 2023 Red Hat.
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


#include "pipe/p_defines.h"
#include "tgsi/tgsi_dump.h"
#include "tgsi/tgsi_parse.h"
#include "util/u_memory.h"
#include "util/u_dump.h"
#include "util/u_string.h"
#include "util/os_time.h"
#include "draw/draw_context.h"

#include "gallivm/lp_bld_debug.h"
#include "gallivm/lp_bld_nir.h"
#include "lp_context.h"
#include "lp_debug.h"
#include "lp_state.h"
#include "lp_perf.h"
#include "lp_state_mesh.h"
#include "lp_screen.h"
#include "util/mesa-sha1.h"
#include "nir_serialize.h"

static struct lp_task_shader_variant_key *
make_task_variant_key(struct llvmpipe_context *lp,
                 struct lp_task_shader *shader,
                 char *store)
{
   struct lp_task_shader_variant_key *key =
      (struct lp_task_shader_variant_key *)store;
   memset(key, 0, sizeof(*key));

   /* This value will be the same for all the variants of a given shader:
    */
   key->nr_samplers = shader->info.base.file_max[TGSI_FILE_SAMPLER] + 1;

   if (shader->info.base.file_max[TGSI_FILE_SAMPLER_VIEW] != -1)
      key->nr_sampler_views = shader->info.base.file_max[TGSI_FILE_SAMPLER_VIEW] + 1;
   struct lp_sampler_static_state *task_sampler;

   task_sampler = lp_task_variant_key_samplers(key);

   memset(task_sampler, 0, MAX2(key->nr_samplers, key->nr_sampler_views) * sizeof *task_sampler);
   for (unsigned i = 0; i < key->nr_samplers; ++i) {
      if (shader->info.base.file_mask[TGSI_FILE_SAMPLER] & (1 << i)) {
         lp_sampler_static_sampler_state(&task_sampler[i].sampler_state,
                                         lp->samplers[PIPE_SHADER_TASK][i]);
      }
   }

   /*
    * XXX If TGSI_FILE_SAMPLER_VIEW exists assume all texture opcodes
    * are dx10-style? Can't really have mixed opcodes, at least not
    * if we want to skip the holes here (without rescanning tgsi).
    */
   if (shader->info.base.file_max[TGSI_FILE_SAMPLER_VIEW] != -1) {
      for (unsigned i = 0; i < key->nr_sampler_views; ++i) {
         /*
          * Note sview may exceed what's representable by file_mask.
          * This will still work, the only downside is that not actually
          * used views may be included in the shader key.
          */
         if ((shader->info.base.file_mask[TGSI_FILE_SAMPLER_VIEW] & (1u << (i & 31))) || i > 31) {
            lp_sampler_static_texture_state(&task_sampler[i].texture_state,
                                            lp->sampler_views[PIPE_SHADER_TASK][i]);
         }
      }
   } else {
      key->nr_sampler_views = key->nr_samplers;
      for (unsigned i = 0; i < key->nr_sampler_views; ++i) {
         if ((shader->info.base.file_mask[TGSI_FILE_SAMPLER] & (1 << i)) || i > 31) {
            lp_sampler_static_texture_state(&task_sampler[i].texture_state,
                                            lp->sampler_views[PIPE_SHADER_TASK][i]);
         }
      }
   }

   struct lp_image_static_state *lp_image;
   lp_image = lp_task_variant_key_images(key);
   key->nr_images = shader->info.base.file_max[TGSI_FILE_IMAGE] + 1;

   if (key->nr_images)
      memset(lp_image, 0,
             key->nr_images * sizeof *lp_image);
   for (unsigned i = 0; i < key->nr_images; ++i) {
      if ((shader->info.base.file_mask[TGSI_FILE_IMAGE] & (1 << i)) || i > 31) {
         lp_sampler_static_texture_state_image(&lp_image[i].image_state,
                                               &lp->images[PIPE_SHADER_TASK][i]);
      }
   }
   return key;
}

static void *
llvmpipe_create_task_state(struct pipe_context *pipe,
                           const struct pipe_shader_state *templ)
{
   struct llvmpipe_context *llvmpipe = llvmpipe_context(pipe);
   struct lp_task_shader *shader = CALLOC_STRUCT(lp_task_shader);
   if (!shader)
      return NULL;

   pipe_reference_init(&shader->reference, 1);
   shader->base.type = templ->type;

   shader->base.ir.nir = templ->ir.nir;
   return shader;
}


static void
llvmpipe_bind_task_state(struct pipe_context *pipe, void *_task)
{
   struct llvmpipe_context *llvmpipe = llvmpipe_context(pipe);

   if (llvmpipe->tss == _task)
      return;

   llvmpipe->tss = (struct lp_task_shader *)_task;
   llvmpipe->dirty |= LP_NEW_TASK;
}

/**
 * Remove shader variant from two lists: the shader's variant list
 * and the context's variant list.
 */
static void
llvmpipe_remove_task_shader_variant(struct llvmpipe_context *lp,
                                    struct lp_task_shader_variant *variant)
{
   if ((LP_DEBUG & DEBUG_MESH) || (gallivm_debug & GALLIVM_DEBUG_IR)) {
      debug_printf("llvmpipe: del task #%u var %u v created %u v cached %u "
                   "v total cached %u inst %u total inst %u\n",
                   variant->shader->no, variant->no,
                   variant->shader->variants_created,
                   variant->shader->variants_cached,
                   lp->nr_task_variants, variant->nr_instrs, lp->nr_task_instrs);
   }

   gallivm_destroy(variant->gallivm);

   /* remove from shader's list */
   list_del(&variant->list_item_local.list);
   variant->shader->variants_cached--;

   /* remove from context's list */
   list_del(&variant->list_item_global.list);
   lp->nr_task_variants--;
   lp->nr_task_instrs -= variant->nr_instrs;

   FREE(variant);
}

static void
llvmpipe_delete_task_state(struct pipe_context *pipe, void *_task)
{
   struct llvmpipe_context *llvmpipe = llvmpipe_context(pipe);
   struct lp_task_shader *shader = _task;
   FREE(shader);
}

static void
generate_task(struct llvmpipe_context *lp,
              struct lp_task_shader *shader,
              struct lp_task_shader_variant *variant)
{
   struct gallivm_state *gallivm = variant->gallivm;
   struct lp_build_tgsi_params params;
   memset(&params, 0, sizeof(params));
   lp_build_nir_soa(gallivm, shader->base.ir.nir, &params,
                    NULL);
}

static void
dump_task_variant_key(const struct lp_task_shader_variant_key *key)
{
   int i;
   debug_printf("task variant %p:\n", (void *) key);

   for (i = 0; i < key->nr_samplers; ++i) {
      const struct lp_sampler_static_state *samplers = lp_task_variant_key_samplers(key);
      const struct lp_static_sampler_state *sampler = &samplers[i].sampler_state;
      debug_printf("sampler[%u] = \n", i);
      debug_printf("  .wrap = %s %s %s\n",
                   util_str_tex_wrap(sampler->wrap_s, TRUE),
                   util_str_tex_wrap(sampler->wrap_t, TRUE),
                   util_str_tex_wrap(sampler->wrap_r, TRUE));
      debug_printf("  .min_img_filter = %s\n",
                   util_str_tex_filter(sampler->min_img_filter, TRUE));
      debug_printf("  .min_mip_filter = %s\n",
                   util_str_tex_mipfilter(sampler->min_mip_filter, TRUE));
      debug_printf("  .mag_img_filter = %s\n",
                   util_str_tex_filter(sampler->mag_img_filter, TRUE));
      if (sampler->compare_mode != PIPE_TEX_COMPARE_NONE)
         debug_printf("  .compare_func = %s\n", util_str_func(sampler->compare_func, TRUE));
      debug_printf("  .normalized_coords = %u\n", sampler->normalized_coords);
      debug_printf("  .min_max_lod_equal = %u\n", sampler->min_max_lod_equal);
      debug_printf("  .lod_bias_non_zero = %u\n", sampler->lod_bias_non_zero);
      debug_printf("  .apply_min_lod = %u\n", sampler->apply_min_lod);
      debug_printf("  .apply_max_lod = %u\n", sampler->apply_max_lod);
      debug_printf("  .aniso = %u\n", sampler->aniso);
   }
   for (i = 0; i < key->nr_sampler_views; ++i) {
      const struct lp_sampler_static_state *samplers = lp_task_variant_key_samplers(key);
      const struct lp_static_texture_state *texture = &samplers[i].texture_state;
      debug_printf("texture[%u] = \n", i);
      debug_printf("  .format = %s\n",
                   util_format_name(texture->format));
      debug_printf("  .target = %s\n",
                   util_str_tex_target(texture->target, TRUE));
      debug_printf("  .level_zero_only = %u\n",
                   texture->level_zero_only);
      debug_printf("  .pot = %u %u %u\n",
                   texture->pot_width,
                   texture->pot_height,
                   texture->pot_depth);
   }
   struct lp_image_static_state *images = lp_task_variant_key_images(key);
   for (i = 0; i < key->nr_images; ++i) {
      const struct lp_static_texture_state *image = &images[i].image_state;
      debug_printf("image[%u] = \n", i);
      debug_printf("  .format = %s\n",
                   util_format_name(image->format));
      debug_printf("  .target = %s\n",
                   util_str_tex_target(image->target, TRUE));
      debug_printf("  .level_zero_only = %u\n",
                   image->level_zero_only);
      debug_printf("  .pot = %u %u %u\n",
                   image->pot_width,
                   image->pot_height,
                   image->pot_depth);
   }
}

static void
lp_debug_task_variant(const struct lp_task_shader_variant *variant)
{
   debug_printf("llvmpipe: Task shader #%u variant #%u:\n",
                variant->shader->no, variant->no);
   nir_print_shader(variant->shader->base.ir.nir, stderr);
   dump_task_variant_key(&variant->key);
   debug_printf("\n");
}

static void
lp_task_get_ir_cache_key(struct lp_task_shader_variant *variant,
                         unsigned char ir_sha1_cache_key[20])
{
   struct blob blob = { 0 };
   unsigned ir_size;
   void *ir_binary;

   blob_init(&blob);
   nir_serialize(&blob, variant->shader->base.ir.nir, true);
   ir_binary = blob.data;
   ir_size = blob.size;

   struct mesa_sha1 ctx;
   _mesa_sha1_init(&ctx);
   _mesa_sha1_update(&ctx, &variant->key, variant->shader->variant_key_size);
   _mesa_sha1_update(&ctx, ir_binary, ir_size);
   _mesa_sha1_final(&ctx, ir_sha1_cache_key);

   blob_finish(&blob);
}

static struct lp_task_shader_variant *
generate_task_variant(struct llvmpipe_context *lp,
                      struct lp_task_shader *shader,
                      const struct lp_task_shader_variant_key *key)
{
   struct llvmpipe_screen *screen = llvmpipe_screen(lp->pipe.screen);

   struct lp_task_shader_variant *variant =
      MALLOC(sizeof *variant + shader->variant_key_size - sizeof variant->key);
   if (!variant)
      return NULL;

   memset(variant, 0, sizeof(*variant));

   char module_name[64];
   snprintf(module_name, sizeof(module_name), "task%u_variant%u",
            shader->no, shader->variants_created);

   variant->shader = shader;
   memcpy(&variant->key, key, shader->variant_key_size); 
   unsigned char ir_sha1_cache_key[20];
   struct lp_cached_code cached = { 0 };
   bool needs_caching = false;
   if (shader->base.ir.nir) {
      lp_task_get_ir_cache_key(variant, ir_sha1_cache_key);

      lp_disk_cache_find_shader(screen, &cached, ir_sha1_cache_key);
      if (!cached.data_size)
         needs_caching = true;
   }

   variant->gallivm = gallivm_create(module_name, lp->context, &cached);
   if (!variant->gallivm) {
      FREE(variant);
      return NULL;
   }

   variant->list_item_global.base = variant;
   variant->list_item_local.base = variant;
   variant->no = shader->variants_created++;

   if ((LP_DEBUG & DEBUG_MESH) || (gallivm_debug & GALLIVM_DEBUG_IR)) {
      lp_debug_task_variant(variant);
   }

   lp_jit_init_task_types(variant);

   generate_task(lp, shader, variant);

   gallivm_compile_module(variant->gallivm);

   variant->nr_instrs += lp_build_count_ir_module(variant->gallivm->module);

   variant->jit_function = (lp_jit_task_func)
      gallivm_jit_function(variant->gallivm, variant->function);

   if (needs_caching) {
      lp_disk_cache_insert_shader(screen, &cached, ir_sha1_cache_key);
   }
   gallivm_free_ir(variant->gallivm);
   return variant;
}


void
llvmpipe_update_task_shader(struct llvmpipe_context *lp)
{
   struct lp_task_shader *shader = lp->tss;

   char store[LP_TASK_MAX_VARIANT_KEY_SIZE];

   const struct lp_task_shader_variant_key *key =
         make_task_variant_key(lp, shader, store);

   struct lp_task_shader_variant *variant = NULL;
   struct lp_task_variant_list_item *li;
   /* Search the variants for one which matches the key */
   LIST_FOR_EACH_ENTRY(li, &shader->variants.list, list) {
      if (memcmp(&li->base->key, key, shader->variant_key_size) == 0) {
         variant = li->base;
         break;
      }
   }

   if (variant) {
      /* Move this variant to the head of the list to implement LRU
       * deletion of shader's when we have too many.
       */
      list_move_to(&variant->list_item_global.list, &lp->fs_variants_list.list);
   } else {
      /* variant not found, create it now */

      if (LP_DEBUG & DEBUG_MESH) {
         debug_printf("%u variants,\t%u instrs,\t%u instrs/variant\n",
                      lp->nr_task_variants,
                      lp->nr_task_instrs,
                      lp->nr_task_variants
                      ? lp->nr_task_instrs / lp->nr_task_variants : 0);
      }

      /* First, check if we've exceeded the max number of shader variants.
       * If so, free 6.25% of them (the least recently used ones).
       */
      unsigned variants_to_cull = lp->nr_task_variants >= LP_MAX_SHADER_VARIANTS
         ? LP_MAX_SHADER_VARIANTS / 16 : 0;

      if (variants_to_cull ||
          lp->nr_task_instrs >= LP_MAX_SHADER_INSTRUCTIONS) {
         if (gallivm_debug & GALLIVM_DEBUG_PERF) {
            debug_printf("Evicting TASK: %u task variants,\t%u total variants,"
                         "\t%u instrs,\t%u instrs/variant\n",
                         shader->variants_cached,
                         lp->nr_task_variants, lp->nr_task_instrs,
                         lp->nr_task_instrs / lp->nr_task_variants);
         }

         /*
          * We need to re-check lp->nr_task_variants because an arbitrarily large
          * number of shader variants (potentially all of them) could be
          * pending for destruction on flush.
          */
         for (unsigned i = 0;
              i < variants_to_cull ||
                 lp->nr_task_instrs >= LP_MAX_SHADER_INSTRUCTIONS; i++) {
            struct lp_task_variant_list_item *item;
            if (list_is_empty(&lp->task_variants_list.list)) {
               break;
            }
            item = list_last_entry(&lp->task_variants_list.list,
                                   struct lp_task_variant_list_item, list);
            assert(item);
            assert(item->base);
            llvmpipe_remove_task_shader_variant(lp, item->base);
         }
      }

      /*
       * Generate the new variant.
       */
      int64_t t0, t1, dt;
      t0 = os_time_get();
      variant = generate_task_variant(lp, shader, key);
      t1 = os_time_get();
      dt = t1 - t0;
      LP_COUNT_ADD(llvm_compile_time, dt);
      LP_COUNT_ADD(nr_llvm_compiles, 2);  /* emit vs. omit in/out test */

      /* Put the new variant into the list */
      if (variant) {
         list_add(&variant->list_item_local.list, &shader->variants.list);
         list_add(&variant->list_item_global.list, &lp->task_variants_list.list);
         lp->nr_task_variants++;
         lp->nr_task_instrs += variant->nr_instrs;
         shader->variants_cached++;
      }
   }
}

static struct lp_mesh_shader_variant_key *
make_mesh_variant_key(struct llvmpipe_context *lp,
                 struct lp_mesh_shader *shader,
                 char *store)
{
   struct lp_mesh_shader_variant_key *key =
      (struct lp_mesh_shader_variant_key *)store;
   memset(key, 0, sizeof(*key));

   /* This value will be the same for all the variants of a given shader:
    */
   key->nr_samplers = shader->info.base.file_max[TGSI_FILE_SAMPLER] + 1;

   if (shader->info.base.file_max[TGSI_FILE_SAMPLER_VIEW] != -1)
      key->nr_sampler_views = shader->info.base.file_max[TGSI_FILE_SAMPLER_VIEW] + 1;
   struct lp_sampler_static_state *mesh_sampler;

   mesh_sampler = lp_mesh_variant_key_samplers(key);

   memset(mesh_sampler, 0, MAX2(key->nr_samplers, key->nr_sampler_views) * sizeof *mesh_sampler);
   for (unsigned i = 0; i < key->nr_samplers; ++i) {
      if (shader->info.base.file_mask[TGSI_FILE_SAMPLER] & (1 << i)) {
         lp_sampler_static_sampler_state(&mesh_sampler[i].sampler_state,
                                         lp->samplers[PIPE_SHADER_MESH][i]);
      }
   }

   /*
    * XXX If TGSI_FILE_SAMPLER_VIEW exists assume all texture opcodes
    * are dx10-style? Can't really have mixed opcodes, at least not
    * if we want to skip the holes here (without rescanning tgsi).
    */
   if (shader->info.base.file_max[TGSI_FILE_SAMPLER_VIEW] != -1) {
      for (unsigned i = 0; i < key->nr_sampler_views; ++i) {
         /*
          * Note sview may exceed what's representable by file_mask.
          * This will still work, the only downside is that not actually
          * used views may be included in the shader key.
          */
         if ((shader->info.base.file_mask[TGSI_FILE_SAMPLER_VIEW] & (1u << (i & 31))) || i > 31) {
            lp_sampler_static_texture_state(&mesh_sampler[i].texture_state,
                                            lp->sampler_views[PIPE_SHADER_MESH][i]);
         }
      }
   } else {
      key->nr_sampler_views = key->nr_samplers;
      for (unsigned i = 0; i < key->nr_sampler_views; ++i) {
         if ((shader->info.base.file_mask[TGSI_FILE_SAMPLER] & (1 << i)) || i > 31) {
            lp_sampler_static_texture_state(&mesh_sampler[i].texture_state,
                                            lp->sampler_views[PIPE_SHADER_MESH][i]);
         }
      }
   }

   struct lp_image_static_state *lp_image;
   lp_image = lp_mesh_variant_key_images(key);
   key->nr_images = shader->info.base.file_max[TGSI_FILE_IMAGE] + 1;

   if (key->nr_images)
      memset(lp_image, 0,
             key->nr_images * sizeof *lp_image);
   for (unsigned i = 0; i < key->nr_images; ++i) {
      if ((shader->info.base.file_mask[TGSI_FILE_IMAGE] & (1 << i)) || i > 31) {
         lp_sampler_static_texture_state_image(&lp_image[i].image_state,
                                               &lp->images[PIPE_SHADER_MESH][i]);
      }
   }
   return key;
}

static void *
llvmpipe_create_mesh_state(struct pipe_context *pipe,
                           const struct pipe_shader_state *templ)
{
   struct llvmpipe_context *llvmpipe = llvmpipe_context(pipe);
   struct lp_mesh_shader *shader = CALLOC_STRUCT(lp_mesh_shader);
   if (!shader)
      return NULL;

   pipe_reference_init(&shader->reference, 1);
   shader->base.type = templ->type;

   shader->base.ir.nir = templ->ir.nir;
   return shader;
}


static void
llvmpipe_bind_mesh_state(struct pipe_context *pipe, void *_mesh)
{
   struct llvmpipe_context *llvmpipe = llvmpipe_context(pipe);

   if (llvmpipe->mhs == _mesh)
      return;

   llvmpipe->mhs = (struct lp_mesh_shader *)_mesh;
   llvmpipe->dirty |= LP_NEW_MESH;
}

/**
 * Remove shader variant from two lists: the shader's variant list
 * and the context's variant list.
 */
static void
llvmpipe_remove_mesh_shader_variant(struct llvmpipe_context *lp,
                                    struct lp_mesh_shader_variant *variant)
{
   if ((LP_DEBUG & DEBUG_MESH) || (gallivm_debug & GALLIVM_DEBUG_IR)) {
      debug_printf("llvmpipe: del mesh #%u var %u v created %u v cached %u "
                   "v total cached %u inst %u total inst %u\n",
                   variant->shader->no, variant->no,
                   variant->shader->variants_created,
                   variant->shader->variants_cached,
                   lp->nr_mesh_variants, variant->nr_instrs, lp->nr_mesh_instrs);
   }

   gallivm_destroy(variant->gallivm);

   /* remove from shader's list */
   list_del(&variant->list_item_local.list);
   variant->shader->variants_cached--;

   /* remove from context's list */
   list_del(&variant->list_item_global.list);
   lp->nr_mesh_variants--;
   lp->nr_mesh_instrs -= variant->nr_instrs;

   FREE(variant);
}

static void
llvmpipe_delete_mesh_state(struct pipe_context *pipe, void *_mesh)
{
   struct llvmpipe_context *llvmpipe = llvmpipe_context(pipe);
   struct lp_mesh_shader *shader = _mesh;
   FREE(shader);
}

static void
generate_mesh(struct llvmpipe_context *lp,
              struct lp_mesh_shader *shader,
              struct lp_mesh_shader_variant *variant)
{
   struct gallivm_state *gallivm = variant->gallivm;
   struct lp_build_tgsi_params params;
   memset(&params, 0, sizeof(params));
   lp_build_nir_soa(gallivm, shader->base.ir.nir, &params,
                    NULL);
}

static void
dump_mesh_variant_key(const struct lp_mesh_shader_variant_key *key)
{
   int i;
   debug_printf("mesh variant %p:\n", (void *) key);

   for (i = 0; i < key->nr_samplers; ++i) {
      const struct lp_sampler_static_state *samplers = lp_mesh_variant_key_samplers(key);
      const struct lp_static_sampler_state *sampler = &samplers[i].sampler_state;
      debug_printf("sampler[%u] = \n", i);
      debug_printf("  .wrap = %s %s %s\n",
                   util_str_tex_wrap(sampler->wrap_s, TRUE),
                   util_str_tex_wrap(sampler->wrap_t, TRUE),
                   util_str_tex_wrap(sampler->wrap_r, TRUE));
      debug_printf("  .min_img_filter = %s\n",
                   util_str_tex_filter(sampler->min_img_filter, TRUE));
      debug_printf("  .min_mip_filter = %s\n",
                   util_str_tex_mipfilter(sampler->min_mip_filter, TRUE));
      debug_printf("  .mag_img_filter = %s\n",
                   util_str_tex_filter(sampler->mag_img_filter, TRUE));
      if (sampler->compare_mode != PIPE_TEX_COMPARE_NONE)
         debug_printf("  .compare_func = %s\n", util_str_func(sampler->compare_func, TRUE));
      debug_printf("  .normalized_coords = %u\n", sampler->normalized_coords);
      debug_printf("  .min_max_lod_equal = %u\n", sampler->min_max_lod_equal);
      debug_printf("  .lod_bias_non_zero = %u\n", sampler->lod_bias_non_zero);
      debug_printf("  .apply_min_lod = %u\n", sampler->apply_min_lod);
      debug_printf("  .apply_max_lod = %u\n", sampler->apply_max_lod);
      debug_printf("  .aniso = %u\n", sampler->aniso);
   }
   for (i = 0; i < key->nr_sampler_views; ++i) {
      const struct lp_sampler_static_state *samplers = lp_mesh_variant_key_samplers(key);
      const struct lp_static_texture_state *texture = &samplers[i].texture_state;
      debug_printf("texture[%u] = \n", i);
      debug_printf("  .format = %s\n",
                   util_format_name(texture->format));
      debug_printf("  .target = %s\n",
                   util_str_tex_target(texture->target, TRUE));
      debug_printf("  .level_zero_only = %u\n",
                   texture->level_zero_only);
      debug_printf("  .pot = %u %u %u\n",
                   texture->pot_width,
                   texture->pot_height,
                   texture->pot_depth);
   }
   struct lp_image_static_state *images = lp_mesh_variant_key_images(key);
   for (i = 0; i < key->nr_images; ++i) {
      const struct lp_static_texture_state *image = &images[i].image_state;
      debug_printf("image[%u] = \n", i);
      debug_printf("  .format = %s\n",
                   util_format_name(image->format));
      debug_printf("  .target = %s\n",
                   util_str_tex_target(image->target, TRUE));
      debug_printf("  .level_zero_only = %u\n",
                   image->level_zero_only);
      debug_printf("  .pot = %u %u %u\n",
                   image->pot_width,
                   image->pot_height,
                   image->pot_depth);
   }
}

static void
lp_debug_mesh_variant(const struct lp_mesh_shader_variant *variant)
{
   debug_printf("llvmpipe: Mesh shader #%u variant #%u:\n",
                variant->shader->no, variant->no);
   nir_print_shader(variant->shader->base.ir.nir, stderr);
   dump_mesh_variant_key(&variant->key);
   debug_printf("\n");
}

static void
lp_mesh_get_ir_cache_key(struct lp_mesh_shader_variant *variant,
                         unsigned char ir_sha1_cache_key[20])
{
   struct blob blob = { 0 };
   unsigned ir_size;
   void *ir_binary;

   blob_init(&blob);
   nir_serialize(&blob, variant->shader->base.ir.nir, true);
   ir_binary = blob.data;
   ir_size = blob.size;

   struct mesa_sha1 ctx;
   _mesa_sha1_init(&ctx);
   _mesa_sha1_update(&ctx, &variant->key, variant->shader->variant_key_size);
   _mesa_sha1_update(&ctx, ir_binary, ir_size);
   _mesa_sha1_final(&ctx, ir_sha1_cache_key);

   blob_finish(&blob);
}

static struct lp_mesh_shader_variant *
generate_mesh_variant(struct llvmpipe_context *lp,
                      struct lp_mesh_shader *shader,
                      const struct lp_mesh_shader_variant_key *key)
{
   struct llvmpipe_screen *screen = llvmpipe_screen(lp->pipe.screen);

   struct lp_mesh_shader_variant *variant =
      MALLOC(sizeof *variant + shader->variant_key_size - sizeof variant->key);
   if (!variant)
      return NULL;

   memset(variant, 0, sizeof(*variant));

   char module_name[64];
   snprintf(module_name, sizeof(module_name), "mesh%u_variant%u",
            shader->no, shader->variants_created);

   variant->shader = shader;
   memcpy(&variant->key, key, shader->variant_key_size); 
   unsigned char ir_sha1_cache_key[20];
   struct lp_cached_code cached = { 0 };
   bool needs_caching = false;
   if (shader->base.ir.nir) {
      lp_mesh_get_ir_cache_key(variant, ir_sha1_cache_key);

      lp_disk_cache_find_shader(screen, &cached, ir_sha1_cache_key);
      if (!cached.data_size)
         needs_caching = true;
   }

   variant->gallivm = gallivm_create(module_name, lp->context, &cached);
   if (!variant->gallivm) {
      FREE(variant);
      return NULL;
   }

   variant->list_item_global.base = variant;
   variant->list_item_local.base = variant;
   variant->no = shader->variants_created++;

   if ((LP_DEBUG & DEBUG_MESH) || (gallivm_debug & GALLIVM_DEBUG_IR)) {
      lp_debug_mesh_variant(variant);
   }

   lp_jit_init_mesh_types(variant);

   generate_mesh(lp, shader, variant);

   gallivm_compile_module(variant->gallivm);

   variant->nr_instrs += lp_build_count_ir_module(variant->gallivm->module);

   variant->jit_function = (lp_jit_mesh_func)
      gallivm_jit_function(variant->gallivm, variant->function);

   if (needs_caching) {
      lp_disk_cache_insert_shader(screen, &cached, ir_sha1_cache_key);
   }
   gallivm_free_ir(variant->gallivm);
   return variant;
}

void
llvmpipe_update_mesh_shader(struct llvmpipe_context *lp)
{
   struct lp_mesh_shader *shader = lp->mhs;

   char store[LP_MESH_MAX_VARIANT_KEY_SIZE];

   const struct lp_mesh_shader_variant_key *key =
         make_mesh_variant_key(lp, shader, store);

   struct lp_mesh_shader_variant *variant = NULL;
   struct lp_mesh_variant_list_item *li;
   /* Search the variants for one which matches the key */
   LIST_FOR_EACH_ENTRY(li, &shader->variants.list, list) {
      if (memcmp(&li->base->key, key, shader->variant_key_size) == 0) {
         variant = li->base;
         break;
      }
   }

   if (variant) {
      /* Move this variant to the head of the list to implement LRU
       * deletion of shader's when we have too many.
       */
      list_move_to(&variant->list_item_global.list, &lp->fs_variants_list.list);
   } else {
      /* variant not found, create it now */

      if (LP_DEBUG & DEBUG_MESH) {
         debug_printf("%u variants,\t%u instrs,\t%u instrs/variant\n",
                      lp->nr_mesh_variants,
                      lp->nr_mesh_instrs,
                      lp->nr_mesh_variants
                      ? lp->nr_mesh_instrs / lp->nr_mesh_variants : 0);
      }

      /* First, check if we've exceeded the max number of shader variants.
       * If so, free 6.25% of them (the least recently used ones).
       */
      unsigned variants_to_cull = lp->nr_mesh_variants >= LP_MAX_SHADER_VARIANTS
         ? LP_MAX_SHADER_VARIANTS / 16 : 0;

      if (variants_to_cull ||
          lp->nr_mesh_instrs >= LP_MAX_SHADER_INSTRUCTIONS) {
         if (gallivm_debug & GALLIVM_DEBUG_PERF) {
            debug_printf("Evicting MESH: %u mesh variants,\t%u total variants,"
                         "\t%u instrs,\t%u instrs/variant\n",
                         shader->variants_cached,
                         lp->nr_mesh_variants, lp->nr_mesh_instrs,
                         lp->nr_mesh_instrs / lp->nr_mesh_variants);
         }

         /*
          * We need to re-check lp->nr_mesh_variants because an arbitrarily large
          * number of shader variants (potentially all of them) could be
          * pending for destruction on flush.
          */
         for (unsigned i = 0;
              i < variants_to_cull ||
                 lp->nr_mesh_instrs >= LP_MAX_SHADER_INSTRUCTIONS; i++) {
            struct lp_mesh_variant_list_item *item;
            if (list_is_empty(&lp->mesh_variants_list.list)) {
               break;
            }
            item = list_last_entry(&lp->mesh_variants_list.list,
                                   struct lp_mesh_variant_list_item, list);
            assert(item);
            assert(item->base);
            llvmpipe_remove_mesh_shader_variant(lp, item->base);
         }
      }

      /*
       * Generate the new variant.
       */
      int64_t t0, t1, dt;
      t0 = os_time_get();
      variant = generate_mesh_variant(lp, shader, key);
      t1 = os_time_get();
      dt = t1 - t0;
      LP_COUNT_ADD(llvm_compile_time, dt);
      LP_COUNT_ADD(nr_llvm_compiles, 2);  /* emit vs. omit in/out test */

      /* Put the new variant into the list */
      if (variant) {
         list_add(&variant->list_item_local.list, &shader->variants.list);
         list_add(&variant->list_item_global.list, &lp->mesh_variants_list.list);
         lp->nr_mesh_variants++;
         lp->nr_mesh_instrs += variant->nr_instrs;
         shader->variants_cached++;
      }
   }
}

static void
llvmpipe_draw_mesh_tasks(struct pipe_context *pipe,
                         const struct pipe_grid_info *info)
{
   struct llvmpipe_context *lp = llvmpipe_context(pipe);

   if (lp->dirty)
      llvmpipe_update_derived(lp);
}

void
llvmpipe_init_mesh_funcs(struct llvmpipe_context *llvmpipe)
{
   llvmpipe->pipe.create_task_state = llvmpipe_create_task_state;
   llvmpipe->pipe.bind_task_state   = llvmpipe_bind_task_state;
   llvmpipe->pipe.delete_task_state = llvmpipe_delete_task_state;
   llvmpipe->pipe.create_mesh_state = llvmpipe_create_mesh_state;
   llvmpipe->pipe.bind_mesh_state   = llvmpipe_bind_mesh_state;
   llvmpipe->pipe.delete_mesh_state = llvmpipe_delete_mesh_state;

   llvmpipe->pipe.draw_mesh_tasks   = llvmpipe_draw_mesh_tasks;
}
