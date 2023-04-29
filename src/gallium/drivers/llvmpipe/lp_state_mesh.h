/**************************************************************************
 *
 * Copyright 2023 Red Hat
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 **************************************************************************/

#ifndef LP_STATE_MESH_H
#define LP_STATE_MESH_H

struct lp_task_shader_variant;

struct lp_task_shader_variant_key
{
   unsigned nr_samplers:8;
   unsigned nr_sampler_views:8;
   unsigned nr_images:8;
};

#define LP_TASK_MAX_VARIANT_KEY_SIZE                                      \
   (sizeof(struct lp_task_shader_variant_key) +                     \
    PIPE_MAX_SHADER_SAMPLER_VIEWS * sizeof(struct lp_sampler_static_state) + \
    PIPE_MAX_SHADER_IMAGES * sizeof(struct lp_image_static_state))


static inline size_t
lp_task_variant_key_size(unsigned nr_samplers, unsigned nr_images)
{
   return (sizeof(struct lp_task_shader_variant_key) +
           nr_samplers * sizeof(struct lp_sampler_static_state) +
           nr_images * sizeof(struct lp_image_static_state));
}

static inline struct lp_sampler_static_state *
lp_task_variant_key_samplers(const struct lp_task_shader_variant_key *key)
{
   return (struct lp_sampler_static_state *)&(key[1]);
}

static inline struct lp_image_static_state *
lp_task_variant_key_images(const struct lp_task_shader_variant_key *key)
{
   return (struct lp_image_static_state *)
      &(lp_task_variant_key_samplers(key)[MAX2(key->nr_samplers, key->nr_sampler_views)]);
}

struct lp_task_variant_list_item
{
   struct list_head list;
   struct lp_task_shader_variant *base;
};

struct lp_task_shader_variant
{
   struct gallivm_state *gallivm;

   LLVMTypeRef jit_task_context_type;
   LLVMTypeRef jit_task_context_ptr_type;
   LLVMTypeRef jit_task_thread_data_type;
   LLVMTypeRef jit_task_thread_data_ptr_type;

   LLVMValueRef function;
   lp_jit_task_func jit_function;

   /* Total number of LLVM instructions generated */
   unsigned nr_instrs;

   struct lp_task_variant_list_item list_item_global, list_item_local;

   struct lp_task_shader *shader;

   /* For debugging/profiling purposes */
   unsigned no;

   /* key is variable-sized, must be last */
   struct lp_task_shader_variant_key key;
};

/** Subclass of pipe_shader_state */
struct lp_task_shader
{
   struct pipe_shader_state base;

   struct pipe_reference reference;

   struct lp_task_variant_list_item variants;
   struct lp_tgsi_info info;
   unsigned variant_key_size;
};

struct lp_mesh_shader_variant;

struct lp_mesh_shader_variant_key
{
   unsigned nr_samplers:8;
   unsigned nr_sampler_views:8;
   unsigned nr_images:8;
};

#define LP_MESH_MAX_VARIANT_KEY_SIZE                                      \
   (sizeof(struct lp_mesh_shader_variant_key) +                     \
    PIPE_MAX_SHADER_SAMPLER_VIEWS * sizeof(struct lp_sampler_static_state) + \
    PIPE_MAX_SHADER_IMAGES * sizeof(struct lp_image_static_state))


static inline size_t
lp_mesh_variant_key_size(unsigned nr_samplers, unsigned nr_images)
{
   return (sizeof(struct lp_mesh_shader_variant_key) +
           nr_samplers * sizeof(struct lp_sampler_static_state) +
           nr_images * sizeof(struct lp_image_static_state));
}

static inline struct lp_sampler_static_state *
lp_mesh_variant_key_samplers(const struct lp_mesh_shader_variant_key *key)
{
   return (struct lp_sampler_static_state *)&(key[1]);
}

static inline struct lp_image_static_state *
lp_mesh_variant_key_images(const struct lp_mesh_shader_variant_key *key)
{
   return (struct lp_image_static_state *)
      &(lp_mesh_variant_key_samplers(key)[MAX2(key->nr_samplers, key->nr_sampler_views)]);
}

struct lp_mesh_variant_list_item
{
   struct list_head list;
   struct lp_mesh_shader_variant *base;
};

struct lp_mesh_shader_variant
{
   struct gallivm_state *gallivm;

   LLVMTypeRef jit_mesh_context_type;
   LLVMTypeRef jit_mesh_context_ptr_type;
   LLVMTypeRef jit_mesh_thread_data_type;
   LLVMTypeRef jit_mesh_thread_data_ptr_type;

   LLVMValueRef function;
   lp_jit_mesh_func jit_function;

   /* Total number of LLVM instructions generated */
   unsigned nr_instrs;

   struct lp_mesh_variant_list_item list_item_global, list_item_local;

   struct lp_mesh_shader *shader;

   /* For debugging/profiling purposes */
   unsigned no;

   /* key is variable-sized, must be last */
   struct lp_mesh_shader_variant_key key;
};

/** Subclass of pipe_shader_state */
struct lp_mesh_shader
{
   struct pipe_shader_state base;

   struct pipe_reference reference;

   struct lp_mesh_variant_list_item variants;
   struct lp_tgsi_info info;
   unsigned variant_key_size;
};

void
llvmpipe_update_task_shader(struct llvmpipe_context *lp);
void
llvmpipe_update_mesh_shader(struct llvmpipe_context *lp);
#endif
