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
