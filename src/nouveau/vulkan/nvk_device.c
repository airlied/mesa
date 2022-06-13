#include "nvk_device.h"

#include "nvk_bo_sync.h"
#include "nvk_cmd_buffer.h"
#include "nvk_instance.h"
#include "nvk_physical_device.h"

#include "nouveau_context.h"
#include "nouveau_push.h"

#include "vulkan/wsi/wsi_common.h"

#include "nvk_cla0c0.h"
#include "cla1c0.h"
#include "nvk_clc3c0.h"

static VkResult
nvk_update_preamble_push(struct nvk_queue_state *qs, struct nvk_device *dev,
                         const struct nvk_queue_alloc_info *needs)
{
   struct nouveau_ws_bo *tls_bo = qs->tls_bo;
   VkResult result;
   if (needs->tls_size > qs->alloc_info.tls_size) {
      tls_bo = nouveau_ws_bo_new(dev->pdev->dev,
                                 needs->tls_size, (1 << 17), NOUVEAU_WS_BO_LOCAL);
      if (!tls_bo) {
         result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
         goto fail;
      }
   }

   if (tls_bo != qs->tls_bo) {
      if (qs->tls_bo)
         nouveau_ws_bo_destroy(qs->tls_bo);
      qs->tls_bo = tls_bo;
   }

   struct nouveau_ws_push *push = nouveau_ws_push_new(dev->pdev->dev, 256);

   nouveau_ws_push_ref(push, qs->tls_bo, NOUVEAU_WS_BO_RDWR);
   P_MTHD(push, NVA0C0, SET_SHADER_LOCAL_MEMORY_A);
   P_NVA0C0_SET_SHADER_LOCAL_MEMORY_A(push, qs->tls_bo->offset >> 32);
   P_NVA0C0_SET_SHADER_LOCAL_MEMORY_B(push, qs->tls_bo->offset & 0xffffffff);

   uint64_t temp_size = qs->tls_bo->size / dev->pdev->dev->mp_count;
   P_MTHD(push, NVA0C0, SET_SHADER_LOCAL_MEMORY_NON_THROTTLED_A);
   P_NVA0C0_SET_SHADER_LOCAL_MEMORY_NON_THROTTLED_A(push, temp_size >> 32);
   P_NVA0C0_SET_SHADER_LOCAL_MEMORY_NON_THROTTLED_B(push, temp_size & ~0x7fff);
   P_NVA0C0_SET_SHADER_LOCAL_MEMORY_NON_THROTTLED_C(push, 0xff);

   if (dev->pdev->compute_class < VOLTA_COMPUTE_A) {
      P_MTHD(push, NVA0C0, SET_SHADER_LOCAL_MEMORY_THROTTLED_A);
      P_NVA0C0_SET_SHADER_LOCAL_MEMORY_THROTTLED_A(push, temp_size >> 32);
      P_NVA0C0_SET_SHADER_LOCAL_MEMORY_THROTTLED_B(push, temp_size & ~0x7fff);
      P_NVA0C0_SET_SHADER_LOCAL_MEMORY_THROTTLED_C(push, 0xff);
   }

   if (dev->pdev->compute_class < VOLTA_COMPUTE_A) {
      P_MTHD(push, NVA0C0, SET_SHADER_LOCAL_MEMORY_WINDOW);
      P_NVA0C0_SET_SHADER_LOCAL_MEMORY_WINDOW(push, 0xff << 24);

      P_MTHD(push, NVA0C0, SET_SHADER_SHARED_MEMORY_WINDOW);
      P_NVA0C0_SET_SHADER_SHARED_MEMORY_WINDOW(push, 0xfe << 24);

      // TODO CODE_ADDRESS_HIGH
   } else {
      uint64_t temp = 0xfeULL << 24;

      P_MTHD(push, NVC3C0, SET_SHADER_SHARED_MEMORY_WINDOW_A);
      P_NVC3C0_SET_SHADER_SHARED_MEMORY_WINDOW_A(push, temp >> 32);
      P_NVC3C0_SET_SHADER_SHARED_MEMORY_WINDOW_B(push, temp & 0xffffffff);

      temp = 0xffULL << 24;
      P_MTHD(push, NVC3C0, SET_SHADER_LOCAL_MEMORY_WINDOW_A);
      P_NVC3C0_SET_SHADER_LOCAL_MEMORY_WINDOW_A(push, temp >> 32);
      P_NVC3C0_SET_SHADER_LOCAL_MEMORY_WINDOW_B(push, temp & 0xffffffff);
   }

   P_MTHD(push, NVA0C0, SET_SPA_VERSION);
   P_NVA0C0_SET_SPA_VERSION(push, { .major = dev->pdev->compute_class >= KEPLER_COMPUTE_B ? 0x4 : 0x3 });

   if (qs->push)
      nouveau_ws_push_destroy(qs->push);
   qs->push = push;
   return 0;
 fail:
   return vk_error(qs, result);
}

static VkResult
nvk_update_preambles(struct nvk_queue_state *qs, struct nvk_device *device,
                     struct vk_command_buffer *const *cmd_buffers, uint32_t cmd_buffer_count)
{
   struct nvk_queue_alloc_info needs = { 0 };
   for (uint32_t i = 0; i < cmd_buffer_count; i++) {
      struct nvk_cmd_buffer *cmd = (struct nvk_cmd_buffer *)cmd_buffers[i];
      needs.tls_size = MAX2(needs.tls_size, cmd->tls_space_needed);
   }

   if (needs.tls_size == qs->alloc_info.tls_size)
      return VK_SUCCESS;
   return nvk_update_preamble_push(qs, device, &needs);
}

static VkResult
nvk_queue_submit(struct vk_queue *vkqueue, struct vk_queue_submit *submission)
{
   struct nvk_device *device = container_of(vkqueue->base.device, struct nvk_device, vk);
   struct nvk_queue *queue = container_of(vkqueue, struct nvk_queue, vk);
   VkResult result;

   result = nvk_update_preambles(&queue->state, device, submission->command_buffers,
                                 submission->command_buffer_count);
   if (result != VK_SUCCESS)
      return result;

   pthread_mutex_lock(&device->mutex);

   if (queue->state.push)
      nouveau_ws_push_submit(queue->state.push, device->pdev->dev, device->ctx);
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

   result = vk_queue_init(&device->queue.vk, &device->vk, &pCreateInfo->pQueueCreateInfos[0], 0);
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

   device->queue.vk.driver_submit = nvk_queue_submit;

   device->pdev = physical_device;

   *pDevice = nvk_device_to_handle(device);

   return VK_SUCCESS;

fail_mutex:
   pthread_mutex_destroy(&device->mutex);
fail_queue:
   vk_queue_finish(&device->queue.vk);
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

   if (device->queue.state.push)
      nouveau_ws_push_destroy(device->queue.state.push);
   if (device->queue.state.tls_bo)
      nouveau_ws_bo_destroy(device->queue.state.tls_bo);
   pthread_cond_destroy(&device->queue_submit);
   pthread_mutex_destroy(&device->mutex);
   vk_queue_finish(&device->queue.vk);
   vk_device_finish(&device->vk);
   nouveau_ws_context_destroy(device->ctx);
   vk_free(&device->vk.alloc, device);
}
