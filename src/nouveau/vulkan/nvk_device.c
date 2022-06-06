#include "nvk_device.h"

#include "nvk_bo_sync.h"
#include "nvk_cmd_buffer.h"
#include "nvk_instance.h"
#include "nvk_physical_device.h"

#include "nouveau_context.h"
#include "nouveau_push.h"

#include "vulkan/wsi/wsi_common.h"

static int
nvc0_screen_resize_tls_area(struct nvk_device *device,
                            uint32_t lpos, uint32_t lneg, uint32_t cstack)
{
   struct nouveau_ws_bo *bo = NULL;
   uint64_t size = (lpos + lneg) * 32 + cstack;

   if (size >= (1 << 20)) {
      return -1;
   }

   size *= 64; /* max warps */
   size  = align(size, 0x8000);
   size *= device->pdev->dev->mp_count;

   size = align(size, 1 << 17);

   bo = nouveau_ws_bo_new(device->pdev->dev, size, (1 << 17), NOUVEAU_WS_BO_LOCAL);
   if (!bo)
      return -1;

   /* Make sure that the pushbuf has acquired a reference to the old tls
    * segment, as it may have commands that will reference it.
    */
   device->tls = bo;
   return 0;
}

static VkResult
nvk_queue_submit(struct vk_queue *queue, struct vk_queue_submit *submission)
{
   struct nvk_device *device = container_of(queue->base.device, struct nvk_device, vk);

   pthread_mutex_lock(&device->mutex);
   for (unsigned i = 0; i < submission->command_buffer_count; i++) {
      struct nvk_cmd_buffer *cmd = (struct nvk_cmd_buffer *)submission->command_buffers[i];

      for (uint32_t i = 0; i < submission->signal_count; i++) {
         struct nvk_bo_sync *bo_sync = container_of(submission->signals[i].sync, struct nvk_bo_sync, sync);
         nouveau_ws_push_ref(cmd->push, bo_sync->bo, NOUVEAU_WS_BO_RDWR);
      }

      nouveau_ws_push_submit(cmd->push, device->pdev->dev, device->ctx);
      if (cmd->reset_on_submit)
         nvk_reset_cmd_buffer(cmd);
   }

   for (uint32_t i = 0; i < submission->signal_count; i++) {
      struct nvk_bo_sync *bo_sync = container_of(submission->signals[i].sync, struct nvk_bo_sync, sync);
      assert(bo_sync->state == NVK_BO_SYNC_STATE_RESET);
      bo_sync->state = NVK_BO_SYNC_STATE_SUBMITTED;
   }

   pthread_cond_broadcast(&device->queue_submit);
   pthread_mutex_unlock(&device->mutex);

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_CreateDevice(VkPhysicalDevice physicalDevice,
   const VkDeviceCreateInfo *pCreateInfo,
   const VkAllocationCallbacks *pAllocator,
   VkDevice *pDevice)
{
   VK_FROM_HANDLE(nvk_physical_device, physical_device, physicalDevice);
   VkResult result = VK_ERROR_OUT_OF_HOST_MEMORY;
   struct nvk_device *device;

   device = vk_zalloc2(&physical_device->instance->vk.alloc,
      pAllocator,
      sizeof(*device),
      8,
      VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!device)
      return vk_error(physical_device, VK_ERROR_OUT_OF_HOST_MEMORY);

   struct vk_device_dispatch_table dispatch_table;
   vk_device_dispatch_table_from_entrypoints(&dispatch_table, &nvk_device_entrypoints, true);
   vk_device_dispatch_table_from_entrypoints(&dispatch_table, &wsi_device_entrypoints, false);

   result =
      vk_device_init(&device->vk, &physical_device->vk, &dispatch_table, pCreateInfo, pAllocator);
   if (result != VK_SUCCESS)
      goto fail_alloc;

   int ret = nouveau_ws_context_create(physical_device->dev, &device->ctx);
   if (ret) {
      if (ret == -ENOSPC)
         result = vk_error(device, VK_ERROR_TOO_MANY_OBJECTS);
      else
         result = vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto fail_init;
   }

   result = vk_queue_init(&device->queue, &device->vk, &pCreateInfo->pQueueCreateInfos[0], 0);
   if (result != VK_SUCCESS)
      goto fail_ctx;

   if (pthread_mutex_init(&device->mutex, NULL) != 0) {
      result = vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
      goto fail_queue;
   }

   pthread_condattr_t condattr;
   if (pthread_condattr_init(&condattr) != 0) {
      result = vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
      goto fail_mutex;
   }
   if (pthread_condattr_setclock(&condattr, CLOCK_MONOTONIC) != 0) {
      pthread_condattr_destroy(&condattr);
      result = vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
      goto fail_mutex;
   }
   if (pthread_cond_init(&device->queue_submit, &condattr) != 0) {
      pthread_condattr_destroy(&condattr);
      result = vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
      goto fail_mutex;
   }
   pthread_condattr_destroy(&condattr);

   device->queue.driver_submit = nvk_queue_submit;

   device->pdev = physical_device;

   if (nvc0_screen_resize_tls_area(device, 128 * 16, 0, 0x200)) {
      result = vk_error(device, VK_ERROR_INITIALIZATION_FAILED);
      goto fail_mutex;
   }

   *pDevice = nvk_device_to_handle(device);

   return VK_SUCCESS;

fail_mutex:
   pthread_mutex_destroy(&device->mutex);
fail_queue:
   vk_queue_finish(&device->queue);
fail_ctx:
   nouveau_ws_context_destroy(device->ctx);
fail_init:
   vk_device_finish(&device->vk);
fail_alloc:
   vk_free(&device->vk.alloc, device);
   return result;
}

VKAPI_ATTR void VKAPI_CALL
nvk_DestroyDevice(VkDevice _device, const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(nvk_device, device, _device);

   if (!device)
      return;

   nouveau_ws_bo_destroy(device->tls);
   pthread_cond_destroy(&device->queue_submit);
   pthread_mutex_destroy(&device->mutex);
   vk_queue_finish(&device->queue);
   vk_device_finish(&device->vk);
   nouveau_ws_context_destroy(device->ctx);
   vk_free(&device->vk.alloc, device);
}
