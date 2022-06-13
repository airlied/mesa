#ifndef NVK_CMD_BUFFER_H
#define NVK_CMD_BUFFER_H 1

#include "nvk_private.h"

#include "nouveau_push.h"

#include "vulkan/runtime/vk_command_buffer.h"
#include "vulkan/runtime/vk_command_pool.h"

#define NVK_CMD_BUF_SIZE 64*1024
#define MAX_SETS 4
#define MAX_BIND_POINTS                2 /* compute + graphics */

struct nvk_cmd_pool {
   struct vk_command_pool vk;
   struct list_head cmd_buffers;
   struct list_head free_cmd_buffers;

   struct nvk_device *dev;
};

struct nvk_cmd_buffer_upload {
   uint8_t *map;
   unsigned offset;
   uint64_t size;
   struct nouveau_ws_bo *upload_bo;
   struct list_head list;
};

struct nvk_descriptor_state {
   struct nvk_descriptor_set *sets[MAX_SETS];
   uint32_t dirty;
   uint32_t valid;
};

struct nvk_cmd_buffer {
   struct vk_command_buffer vk;

   struct nvk_cmd_pool *pool;
   struct list_head pool_link;

   struct nouveau_ws_push *push;
   bool reset_on_submit;

   struct nvk_cmd_buffer_upload upload;

   VkResult record_result;
   struct nvk_compute_pipeline *cp;

   struct nvk_descriptor_state descriptors[MAX_BIND_POINTS];

   uint64_t tls_space_needed;
};

VkResult nvk_reset_cmd_buffer(struct nvk_cmd_buffer *cmd_buffer);

VK_DEFINE_HANDLE_CASTS(nvk_cmd_buffer, vk.base, VkCommandBuffer,
                       VK_OBJECT_TYPE_COMMAND_BUFFER)
VK_DEFINE_NONDISP_HANDLE_CASTS(nvk_cmd_pool, vk.base, VkCommandPool,
                               VK_OBJECT_TYPE_COMMAND_POOL)

static inline struct nvk_descriptor_state *
nvk_get_descriptors_state(struct nvk_cmd_buffer *cmd_buffer, VkPipelineBindPoint bind_point)
{
   switch (bind_point) {
   case VK_PIPELINE_BIND_POINT_GRAPHICS:
   case VK_PIPELINE_BIND_POINT_COMPUTE:
      return &cmd_buffer->descriptors[bind_point];
   default:
      unreachable("Unhandled bind point");
   }
};

bool
nvk_cmd_buffer_upload_alloc(struct nvk_cmd_buffer *cmd_buffer, unsigned size,
                            unsigned *out_offset, void **ptr);

#endif
