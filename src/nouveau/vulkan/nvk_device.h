#ifndef NVK_DEVICE_H
#define NVK_DEVICE_H 1

#include "nvk_private.h"

#include "vulkan/runtime/vk_device.h"
#include "vulkan/runtime/vk_queue.h"

struct novueau_ws_context;
struct nvk_physical_device;

struct nvk_queue_alloc_info {
   uint64_t tls_size;
};

struct nvk_queue_state {
   struct nvk_queue_alloc_info alloc_info;
   struct nouveau_ws_bo *tls_bo;

   struct nouveau_ws_push *push;
};

struct nvk_queue {
   struct vk_queue vk;

   struct nvk_queue_state state;
};


struct nvk_device {
   struct vk_device vk;
   struct nvk_physical_device *pdev;

   struct nouveau_ws_context *ctx;

   struct nvk_queue queue;

   pthread_mutex_t mutex;
   pthread_cond_t queue_submit;
};

VK_DEFINE_HANDLE_CASTS(nvk_device, vk.base, VkDevice, VK_OBJECT_TYPE_DEVICE)

static struct nvk_physical_device *
nvk_device_physical(struct nvk_device *device)
{
   return (struct nvk_physical_device *)device->vk.physical;
}

#endif
