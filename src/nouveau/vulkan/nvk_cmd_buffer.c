#include "nvk_cmd_buffer.h"

#include "nvk_buffer.h"
#include "nvk_descriptor_set.h"
#include "nvk_descriptor_set_layout.h"
#include "nvk_device.h"
#include "nvk_device_memory.h"
#include "nvk_pipeline.h"
#include "nvk_pipeline_layout.h"
#include "nvk_physical_device.h"

#include "nouveau_push.h"
#include "nouveau_context.h"

#include "nouveau/nouveau.h"

#include "cla1c0.h"
#include "nvk_cla0c0.h"
#include "nvk_clc3c0.h"

#include "drf.h"
#include "cla0c0qmd.h"
#include "clc0c0qmd.h"
#include "clc3c0qmd.h"

#define NVA0C0_QMDV00_06_VAL_SET(p,a...) NVVAL_MW_SET((p), NVA0C0, QMDV00_06, ##a)
#define NVA0C0_QMDV00_06_DEF_SET(p,a...) NVDEF_MW_SET((p), NVA0C0, QMDV00_06, ##a)
#define NVC0C0_QMDV02_01_VAL_SET(p,a...) NVVAL_MW_SET((p), NVC0C0, QMDV02_01, ##a)
#define NVC0C0_QMDV02_01_DEF_SET(p,a...) NVDEF_MW_SET((p), NVC0C0, QMDV02_01, ##a)
#define NVC3C0_QMDV02_02_VAL_SET(p,a...) NVVAL_MW_SET((p), NVC3C0, QMDV02_02, ##a)
#define NVC3C0_QMDV02_02_DEF_SET(p,a...) NVDEF_MW_SET((p), NVC3C0, QMDV02_02, ##a)

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

static bool
nvk_cmd_buffer_resize_upload_buf(struct nvk_cmd_buffer *cmd_buffer, uint64_t min_needed)
{
   uint64_t new_size;
   struct nouveau_ws_bo *bo = NULL;
   struct nvk_cmd_buffer_upload *upload;
   struct nvk_device *device = (struct nvk_device *)cmd_buffer->vk.base.device;

   new_size = MAX2(min_needed, 16 * 1024);
   new_size = MAX2(new_size, 2 * cmd_buffer->upload.size);

   uint32_t flags = NOUVEAU_WS_BO_GART | NOUVEAU_WS_BO_MAP;
   bo = nouveau_ws_bo_new(device->pdev->dev, new_size, 0, flags);

   nouveau_ws_push_ref(cmd_buffer->push, bo, NOUVEAU_WS_BO_RD);
   if (cmd_buffer->upload.upload_bo) {
      upload = malloc(sizeof(*upload));

      if (!upload) {
         cmd_buffer->record_result = VK_ERROR_OUT_OF_HOST_MEMORY;
         nouveau_ws_bo_destroy(bo);
         return false;
      }

      memcpy(upload, &cmd_buffer->upload, sizeof(*upload));
      list_add(&upload->list, &cmd_buffer->upload.list);
   }

   cmd_buffer->upload.upload_bo = bo;
   cmd_buffer->upload.size = new_size;
   cmd_buffer->upload.offset = 0;
   cmd_buffer->upload.map = nouveau_ws_bo_map(cmd_buffer->upload.upload_bo, NOUVEAU_WS_BO_WR);

   if (!cmd_buffer->upload.map) {
      cmd_buffer->record_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      return false;
   }

   return true;
}

bool
nvk_cmd_buffer_upload_alloc(struct nvk_cmd_buffer *cmd_buffer, unsigned size,
                             unsigned *out_offset, void **ptr)
{
   assert(size % 4 == 0);

   /* Align to the scalar cache line size if it results in this allocation
    * being placed in less of them.
    */
   unsigned offset = cmd_buffer->upload.offset;
   unsigned line_size = 256;//for compute dispatches
   unsigned gap = align(offset, line_size) - offset;
   if ((size & ~(line_size - 1)) > gap)
      offset = align(offset, line_size);

   if (offset + size > cmd_buffer->upload.size) {
      if (!nvk_cmd_buffer_resize_upload_buf(cmd_buffer, size))
         return false;
      offset = 0;
   }

   *out_offset = offset;
   *ptr = cmd_buffer->upload.map + offset;

   cmd_buffer->upload.offset = offset + size;
   return true;
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

static void nve4_begin_compute(struct nvk_cmd_buffer *cmd)
{
   struct nvk_device *dev = (struct nvk_device *)cmd->vk.base.device;
   struct nvk_physical_device *pdev = dev->pdev;

   nouveau_ws_push_ref(cmd->push, dev->tls, NOUVEAU_WS_BO_RDWR);
   P_MTHD(cmd->push, NVA0C0, SET_SHADER_LOCAL_MEMORY_A);
   P_NVA0C0_SET_SHADER_LOCAL_MEMORY_A(cmd->push, dev->tls->offset >> 32);
   P_NVA0C0_SET_SHADER_LOCAL_MEMORY_B(cmd->push, dev->tls->offset & 0xffffffff);

   /* No idea why there are 2. Divide size by 2 to be safe.
    * Actually this might be per-MP TEMP size and looks like I'm only using
    * 2 MPs instead of all 8.
    */
   uint64_t temp_size = dev->tls->size / dev->pdev->dev->mp_count;
   P_MTHD(cmd->push, NVA0C0, SET_SHADER_LOCAL_MEMORY_NON_THROTTLED_A);
   P_NVA0C0_SET_SHADER_LOCAL_MEMORY_NON_THROTTLED_A(cmd->push, temp_size >> 32);
   P_NVA0C0_SET_SHADER_LOCAL_MEMORY_NON_THROTTLED_B(cmd->push, temp_size & ~0x7fff);
   P_NVA0C0_SET_SHADER_LOCAL_MEMORY_NON_THROTTLED_C(cmd->push, 0xff);

   if (pdev->compute_class < VOLTA_COMPUTE_A) {
      P_MTHD(cmd->push, NVA0C0, SET_SHADER_LOCAL_MEMORY_THROTTLED_A);
      P_NVA0C0_SET_SHADER_LOCAL_MEMORY_THROTTLED_A(cmd->push, temp_size >> 32);
      P_NVA0C0_SET_SHADER_LOCAL_MEMORY_THROTTLED_B(cmd->push, temp_size & ~0x7fff);
      P_NVA0C0_SET_SHADER_LOCAL_MEMORY_THROTTLED_C(cmd->push, 0xff);
   }

   if (pdev->compute_class < VOLTA_COMPUTE_A) {
      P_MTHD(cmd->push, NVA0C0, SET_SHADER_LOCAL_MEMORY_WINDOW);
      P_NVA0C0_SET_SHADER_LOCAL_MEMORY_WINDOW(cmd->push, 0xff << 24);

      P_MTHD(cmd->push, NVA0C0, SET_SHADER_SHARED_MEMORY_WINDOW);
      P_NVA0C0_SET_SHADER_SHARED_MEMORY_WINDOW(cmd->push, 0xfe << 24);

      // TODO CODE_ADDRESS_HIGH
   } else {
      uint64_t temp = 0xfeULL << 24;

      P_MTHD(cmd->push, NVC3C0, SET_SHADER_SHARED_MEMORY_WINDOW_A);
      P_NVC3C0_SET_SHADER_SHARED_MEMORY_WINDOW_A(cmd->push, temp >> 32);
      P_NVC3C0_SET_SHADER_SHARED_MEMORY_WINDOW_B(cmd->push, temp & 0xffffffff);

      temp = 0xffULL << 24;
      P_MTHD(cmd->push, NVC3C0, SET_SHADER_LOCAL_MEMORY_WINDOW_A);
      P_NVC3C0_SET_SHADER_LOCAL_MEMORY_WINDOW_A(cmd->push, temp >> 32);
      P_NVC3C0_SET_SHADER_LOCAL_MEMORY_WINDOW_B(cmd->push, temp & 0xffffffff);
   }

   P_MTHD(cmd->push, NVA0C0, SET_SPA_VERSION);
   P_NVA0C0_SET_SPA_VERSION(cmd->push, { .major = pdev->compute_class >= KEPLER_COMPUTE_B ? 0x4 : 0x3 });

   P_MTHD(cmd->push, NVA0C0, INVALIDATE_SHADER_CACHES_NO_WFI);
   P_NVA0C0_INVALIDATE_SHADER_CACHES_NO_WFI(cmd->push, { .constant = CONSTANT_TRUE });
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_BeginCommandBuffer(VkCommandBuffer commandBuffer, const VkCommandBufferBeginInfo *pBeginInfo)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);

   if (pBeginInfo->flags & VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT)
      cmd->reset_on_submit = true;
   else
      cmd->reset_on_submit = false;

   struct nvk_device *dev = (struct nvk_device *)cmd->vk.base.device;
   struct nvk_physical_device *pdev = dev->pdev;

   if (pdev->dev->chipset >= 0xe0)
      nve4_begin_compute(cmd);

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

static void
nvk_set_descriptor_set(struct nvk_cmd_buffer *cmd_buffer, VkPipelineBindPoint bind_point,
                       struct nvk_descriptor_set *set, unsigned idx)
{
   struct nvk_descriptor_state *descriptors_state =
      nvk_get_descriptors_state(cmd_buffer, bind_point);

   descriptors_state->sets[idx] = set;

   descriptors_state->valid |= (1u << idx); /* active descriptors */
   descriptors_state->dirty |= (1u << idx);
}

static void
nvk_bind_descriptor_set(struct nvk_cmd_buffer *cmd_buffer, VkPipelineBindPoint bind_point,
                        struct nvk_descriptor_set *set, unsigned idx)
{

   nvk_set_descriptor_set(cmd_buffer, bind_point, set, idx);

   if (set->bo)
      nouveau_ws_push_ref(cmd_buffer->push, set->bo, NOUVEAU_WS_BO_RD);
}

VKAPI_ATTR void VKAPI_CALL
nvk_CmdBindPipeline(
    VkCommandBuffer                             commandBuffer,
    VkPipelineBindPoint                         pipelineBindPoint,
    VkPipeline                                  _pipeline)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);
   VK_FROM_HANDLE(nvk_pipeline, pipeline, _pipeline);

   if (pipelineBindPoint == VK_PIPELINE_BIND_POINT_COMPUTE) {
      cmd->cp = (struct nvk_compute_pipeline *)pipeline;

      nouveau_ws_push_ref(cmd->push, pipeline->shaders[MESA_SHADER_COMPUTE].bo, NOUVEAU_WS_BO_RD);
   }
}

VKAPI_ATTR void VKAPI_CALL
nvk_CmdBindDescriptorSets(
    VkCommandBuffer                             commandBuffer,
    VkPipelineBindPoint                         pipelineBindPoint,
    VkPipelineLayout                            _layout,
    uint32_t                                    firstSet,
    uint32_t                                    descriptorSetCount,
    const VkDescriptorSet*                      pDescriptorSets,
    uint32_t                                    dynamicOffsetCount,
    const uint32_t*                             pDynamicOffsets)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd_buffer, commandBuffer);

   for (unsigned i = 0; i < descriptorSetCount; ++i) {
      unsigned set_idx = i + firstSet;
      VK_FROM_HANDLE(nvk_descriptor_set, set, pDescriptorSets[i]);

      nvk_bind_descriptor_set(cmd_buffer, pipelineBindPoint, set, set_idx);
   }
}

static void
gv100_compute_setup_launch_desc(uint32_t *qmd,
                                uint32_t x, uint32_t y, uint32_t z)
{
   NVC3C0_QMDV02_02_VAL_SET(qmd, CTA_RASTER_WIDTH, x);
   NVC3C0_QMDV02_02_VAL_SET(qmd, CTA_RASTER_HEIGHT, y);
   NVC3C0_QMDV02_02_VAL_SET(qmd, CTA_RASTER_DEPTH, z);
}

static inline void
gp100_cp_launch_desc_set_cb(uint32_t *qmd, unsigned index, uint32_t size, uint64_t address)
{
   NVC0C0_QMDV02_01_VAL_SET(qmd, CONSTANT_BUFFER_ADDR_LOWER, index, address);
   NVC0C0_QMDV02_01_VAL_SET(qmd, CONSTANT_BUFFER_ADDR_UPPER, index, address >> 32);
   NVC0C0_QMDV02_01_VAL_SET(qmd, CONSTANT_BUFFER_SIZE_SHIFTED4, index,
                                 DIV_ROUND_UP(size, 16));
   NVC0C0_QMDV02_01_DEF_SET(qmd, CONSTANT_BUFFER_VALID, index, TRUE);
}


VKAPI_ATTR void VKAPI_CALL
nvk_CmdDispatch(
    VkCommandBuffer                             commandBuffer,
    uint32_t                                    groupCountX,
    uint32_t                                    groupCountY,
    uint32_t                                    groupCountZ)
{
   VK_FROM_HANDLE(nvk_cmd_buffer, cmd, commandBuffer);
   uint32_t *qmd;
   uint32_t qmd_offset;

   struct nvk_descriptor_state *desc = nvk_get_descriptors_state(cmd, VK_PIPELINE_BIND_POINT_COMPUTE);

   /* bind the descriptor to the ubo */
   uint32_t scratch_len_dw = 3 + 3 + 1 + 1; /* grid, block, 0, work dim */
   scratch_len_dw += util_bitcount(desc->valid) * 2;
   uint32_t *scratch;
   uint32_t scratch_offset;
   uint64_t scratch_base = 0;
   if (!nvk_cmd_buffer_upload_alloc(cmd, scratch_len_dw * 4, &scratch_offset, (void **)&scratch))
         return;

   scratch_base = cmd->upload.upload_bo->offset + scratch_offset;

   uint32_t grid[3] = { groupCountX, groupCountY, groupCountZ };

   P_MTHD(cmd->push, NVA0C0, OFFSET_OUT_UPPER);
   P_NVA0C0_OFFSET_OUT_UPPER(cmd->push, scratch_base >> 32);
   P_NVA0C0_OFFSET_OUT(cmd->push, scratch_base & 0xffffffff);
   P_MTHD(cmd->push, NVA0C0, LINE_LENGTH_IN);
   P_NVA0C0_LINE_LENGTH_IN(cmd->push, scratch_len_dw * 4);
   P_NVA0C0_LINE_COUNT(cmd->push, 0x1);

   P_1INC(cmd->push, NVA0C0, LAUNCH_DMA);
   P_NVA0C0_LAUNCH_DMA(cmd->push,
                       { .dst_memory_layout = DST_MEMORY_LAYOUT_PITCH,
                         .sysmembar_disable = SYSMEMBAR_DISABLE_TRUE });
   P_INLINE_ARRAY(cmd->push, cmd->cp->base.shaders[MESA_SHADER_COMPUTE].cp.block_size, 3);
   P_INLINE_ARRAY(cmd->push, grid, 3);
   P_INLINE_DATA(cmd->push, 0);
   P_INLINE_DATA(cmd->push, 0);
   unsigned sets = desc->valid;
   while (sets) {
      int set = u_bit_scan(&sets);
      uint64_t set_addr = desc->sets[set]->bo->offset + desc->sets[set]->bo_offset;
      P_INLINE_DATA(cmd->push, set_addr & 0xffffffff);
      P_INLINE_DATA(cmd->push, set_addr >> 32);
   }

   if (!nvk_cmd_buffer_upload_alloc(cmd, 512, &qmd_offset, (void **)&qmd))
         return;

   memcpy(qmd, cmd->cp->qmd_template, 256);
   gv100_compute_setup_launch_desc(qmd, groupCountX, groupCountY, groupCountZ);

   gp100_cp_launch_desc_set_cb(qmd, 1, 256, scratch_base);
   uint64_t desc_gpuaddr = cmd->upload.upload_bo->offset + qmd_offset;

   P_MTHD(cmd->push, NVA0C0, INVALIDATE_SHADER_CACHES_NO_WFI);
   P_NVA0C0_INVALIDATE_SHADER_CACHES_NO_WFI(cmd->push, { .constant = CONSTANT_TRUE });

   P_MTHD(cmd->push, NVA0C0, SEND_PCAS_A);
   P_NVA0C0_SEND_PCAS_A(cmd->push, desc_gpuaddr >> 8);
   P_IMMD(cmd->push, NVA0C0, SEND_SIGNALING_PCAS_B,
          { .invalidate = INVALIDATE_TRUE,
            .schedule = SCHEDULE_TRUE });
}
