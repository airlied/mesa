#include "nvk_cmd_buffer.h"

#include "nvk_device.h"
#include "nvk_physical_device.h"

#include "nouveau_push.h"

static void
nvk_destroy_cmd_buffer(struct nvk_cmd_buffer *cmd_buffer)
{
   list_del(&cmd_buffer->pool_link);

   nouveau_ws_push_destroy(cmd_buffer->push);
   vk_command_buffer_finish(&cmd_buffer->vk);
   vk_free(&cmd_buffer->pool->vk.alloc, cmd_buffer);
}

static VkResult
nvk_create_cmd_buffer(struct nvk_device *device, struct nvk_cmd_pool *pool,
                      VkCommandBufferLevel level, VkCommandBuffer *pCommandBuffer)
{
   struct nvk_cmd_buffer *cmd_buffer;

   cmd_buffer = vk_zalloc(&pool->vk.alloc, sizeof(*cmd_buffer), 8,
                          VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (cmd_buffer == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   VkResult result =
      vk_command_buffer_init(&cmd_buffer->vk, &pool->vk, level);
   if (result != VK_SUCCESS) {
      vk_free(&cmd_buffer->pool->vk.alloc, cmd_buffer);
      return result;
   }

   cmd_buffer->pool = pool;
   list_addtail(&cmd_buffer->pool_link, &pool->cmd_buffers);

   cmd_buffer->push = nouveau_ws_push_new(device->pdev->dev, NVK_CMD_BUF_SIZE);
   *pCommandBuffer = nvk_cmd_buffer_to_handle(cmd_buffer);
   return VK_SUCCESS;
}

VkResult
nvk_reset_cmd_buffer(struct nvk_cmd_buffer *cmd_buffer)
{
   vk_command_buffer_reset(&cmd_buffer->vk);

   nouveau_ws_push_reset(cmd_buffer->push);

   cmd_buffer->record_result = VK_SUCCESS;

   return cmd_buffer->record_result;
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_CreateCommandPool(VkDevice _device, const VkCommandPoolCreateInfo *pCreateInfo,
                      const VkAllocationCallbacks *pAllocator, VkCommandPool *pCmdPool)
{
   VK_FROM_HANDLE(nvk_device, device, _device);
   struct nvk_cmd_pool *pool;

   pool =
      vk_alloc2(&device->vk.alloc, pAllocator, sizeof(*pool), 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (pool == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   VkResult result = vk_command_pool_init(&pool->vk, &device->vk, pCreateInfo, pAllocator);
   if (result != VK_SUCCESS) {
      vk_free2(&device->vk.alloc, pAllocator, pool);
      return result;
   }

   list_inithead(&pool->cmd_buffers);
   list_inithead(&pool->free_cmd_buffers);
   pool->dev = device;

   *pCmdPool = nvk_cmd_pool_to_handle(pool);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
nvk_DestroyCommandPool(VkDevice _device, VkCommandPool commandPool,
                       const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(nvk_device, device, _device);
   VK_FROM_HANDLE(nvk_cmd_pool, pool, commandPool);

   if (!pool)
      return;

   list_for_each_entry_safe(struct nvk_cmd_buffer, cmd_buffer, &pool->cmd_buffers, pool_link)
   {
      nvk_destroy_cmd_buffer(cmd_buffer);
   }

   list_for_each_entry_safe(struct nvk_cmd_buffer, cmd_buffer, &pool->free_cmd_buffers, pool_link)
   {
      nvk_destroy_cmd_buffer(cmd_buffer);
   }

   vk_command_pool_finish(&pool->vk);
   vk_free2(&device->vk.alloc, pAllocator, pool);
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_ResetCommandPool(VkDevice device, VkCommandPool commandPool, VkCommandPoolResetFlags flags)
{
   VK_FROM_HANDLE(nvk_cmd_pool, pool, commandPool);
   VkResult result;

   list_for_each_entry(struct nvk_cmd_buffer, cmd_buffer, &pool->cmd_buffers, pool_link)
   {
      result = nvk_reset_cmd_buffer(cmd_buffer);
      if (result != VK_SUCCESS)
         return result;
   }

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
nvk_TrimCommandPool(VkDevice device, VkCommandPool commandPool, VkCommandPoolTrimFlags flags)
{
   VK_FROM_HANDLE(nvk_cmd_pool, pool, commandPool);

   list_for_each_entry_safe(struct nvk_cmd_buffer, cmd_buffer, &pool->free_cmd_buffers, pool_link)
   {
      nvk_destroy_cmd_buffer(cmd_buffer);
   }
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_AllocateCommandBuffers(VkDevice _device,
                           const VkCommandBufferAllocateInfo *pAllocateInfo,
                           VkCommandBuffer *pCommandBuffers)
{
   VK_FROM_HANDLE(nvk_device, device, _device);
   VK_FROM_HANDLE(nvk_cmd_pool, pool, pAllocateInfo->commandPool);
   uint32_t i;
   VkResult result = VK_SUCCESS;

   for (i = 0; i < pAllocateInfo->commandBufferCount; i++) {
      if (!list_is_empty(&pool->free_cmd_buffers)) {
         struct nvk_cmd_buffer *cmd_buffer =
            list_first_entry(&pool->free_cmd_buffers, struct nvk_cmd_buffer, pool_link);

         list_del(&cmd_buffer->pool_link);
         list_addtail(&cmd_buffer->pool_link, &pool->cmd_buffers);

         result = nvk_reset_cmd_buffer(cmd_buffer);
         vk_command_buffer_finish(&cmd_buffer->vk);
         VkResult init_result =
            vk_command_buffer_init(&cmd_buffer->vk, &pool->vk, pAllocateInfo->level);
         if (init_result != VK_SUCCESS)
            result = init_result;

         pCommandBuffers[i] = nvk_cmd_buffer_to_handle(cmd_buffer);
      } else {
         result = nvk_create_cmd_buffer(device, pool, pAllocateInfo->level, &pCommandBuffers[i]);
      }
      if (result != VK_SUCCESS)
         break;
   }

   if (result != VK_SUCCESS) {
      nvk_FreeCommandBuffers(_device, pAllocateInfo->commandPool, i, pCommandBuffers);
      /* From the Vulkan 1.0.66 spec:
       *
       * "vkAllocateCommandBuffers can be used to create multiple
       *  command buffers. If the creation of any of those command
       *  buffers fails, the implementation must destroy all
       *  successfully created command buffer objects from this
       *  command, set all entries of the pCommandBuffers array to
       *  NULL and return the error."
       */
      memset(pCommandBuffers, 0, sizeof(*pCommandBuffers) * pAllocateInfo->commandBufferCount);
   }
   return result;
}

VKAPI_ATTR void VKAPI_CALL
nvk_FreeCommandBuffers(VkDevice device, VkCommandPool commandPool, uint32_t commandBufferCount,
                       const VkCommandBuffer *pCommandBuffers)
{
   VK_FROM_HANDLE(nvk_cmd_pool, pool, commandPool);
   for (uint32_t i = 0; i < commandBufferCount; i++) {
      VK_FROM_HANDLE(nvk_cmd_buffer, cmd_buffer, pCommandBuffers[i]);

      if (!cmd_buffer)
         continue;
      assert(cmd_buffer->pool == pool);

      list_del(&cmd_buffer->pool_link);
      list_addtail(&cmd_buffer->pool_link, &pool->free_cmd_buffers);
   }
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_ResetCommandBuffer(VkCommandBuffer commandBuffer, VkCommandBufferResetFlags flags)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd_buffer, commandBuffer);
   return nvk_reset_cmd_buffer(cmd_buffer);
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_BeginCommandBuffer(VkCommandBuffer commandBuffer, const VkCommandBufferBeginInfo *pBeginInfo)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);

   if (pBeginInfo->flags & VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT)
      cmd->reset_on_submit = true;
   else
      cmd->reset_on_submit = false;

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_EndCommandBuffer(VkCommandBuffer commandBuffer)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);
   return cmd->record_result;
}

VKAPI_ATTR void VKAPI_CALL
nvk_CmdPipelineBarrier2(VkCommandBuffer commandBuffer, const VkDependencyInfo *pDependencyInfo) {
}
