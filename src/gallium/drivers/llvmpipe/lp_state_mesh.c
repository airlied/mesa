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
#include "draw/draw_context.h"

#include "lp_context.h"
#include "lp_debug.h"
#include "lp_state.h"
#include "lp_state_mesh.h"

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


static void
llvmpipe_delete_task_state(struct pipe_context *pipe, void *_task)
{
   struct llvmpipe_context *llvmpipe = llvmpipe_context(pipe);
   struct lp_task_shader *shader = _task;
   FREE(shader);
}

void
llvmpipe_update_task_shader(struct llvmpipe_context *lp)
{

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


static void
llvmpipe_delete_mesh_state(struct pipe_context *pipe, void *_mesh)
{
   struct llvmpipe_context *llvmpipe = llvmpipe_context(pipe);
   struct lp_mesh_shader *shader = _mesh;
   FREE(shader);
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
