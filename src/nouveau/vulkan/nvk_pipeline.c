
#include "nvk_private.h"
#include "nvk_device.h"
#include "nvk_pipeline.h"
#include "nvk_pipeline_layout.h"

#include "vk_pipeline_cache.h"

static void
nvk_pipeline_destroy(struct nvk_device *device,
                     struct nvk_pipeline *pipeline,
                     const VkAllocationCallbacks *pAllocator)
{
   vk_object_free(&device->vk, pAllocator, pipeline);
}

static VkResult
nvk_graphics_pipeline_create(struct nvk_device *device,
                             struct vk_pipeline_cache *cache,
                             const VkGraphicsPipelineCreateInfo *pCreateInfo,
                             const VkAllocationCallbacks *pAllocator,
                             VkPipeline *pPipeline)
{
   unreachable("Graphics pipelines not yet implemented");
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_CreateGraphicsPipelines(VkDevice _device,
                            VkPipelineCache pipelineCache,
                            uint32_t createInfoCount,
                            const VkGraphicsPipelineCreateInfo *pCreateInfos,
                            const VkAllocationCallbacks *pAllocator,
                            VkPipeline *pPipelines)
{
   VK_FROM_HANDLE(nvk_device, device, _device);
   VK_FROM_HANDLE(vk_pipeline_cache, cache, pipelineCache);
   VkResult result = VK_SUCCESS;

   unsigned i = 0;
   for (; i < createInfoCount; i++) {
      VkResult r = nvk_graphics_pipeline_create(device, cache, &pCreateInfos[i],
                                                pAllocator, &pPipelines[i]);
      if (r == VK_SUCCESS)
         continue;

      result = r;
      if (pCreateInfos[i].flags & VK_PIPELINE_CREATE_EARLY_RETURN_ON_FAILURE_BIT)
         break;
   }

   for (; i < createInfoCount; i++)
      pPipelines[i] = VK_NULL_HANDLE;

   return result;
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_CreateComputePipelines(VkDevice _device,
                           VkPipelineCache pipelineCache,
                           uint32_t createInfoCount,
                           const VkComputePipelineCreateInfo *pCreateInfos,
                           const VkAllocationCallbacks *pAllocator,
                           VkPipeline *pPipelines)
{
   VK_FROM_HANDLE(nvk_device, device, _device);
   VK_FROM_HANDLE(vk_pipeline_cache, cache, pipelineCache);
   VkResult result = VK_SUCCESS;

   unsigned i = 0;
   for (; i < createInfoCount; i++) {
      VkResult r = nvk_compute_pipeline_create(device, cache, &pCreateInfos[i],
                                               pAllocator, &pPipelines[i]);
      if (r == VK_SUCCESS)
         continue;

      result = r;
      if (pCreateInfos[i].flags & VK_PIPELINE_CREATE_EARLY_RETURN_ON_FAILURE_BIT)
         break;
   }

   for (; i < createInfoCount; i++)
      pPipelines[i] = VK_NULL_HANDLE;

   return result;
}

VKAPI_ATTR void VKAPI_CALL
nvk_DestroyPipeline(VkDevice _device, VkPipeline _pipeline,
                    const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(nvk_device, device, _device);
   VK_FROM_HANDLE(nvk_pipeline, pipeline, _pipeline);

   if (!pipeline)
      return;

   nvk_pipeline_destroy(device, pipeline, pAllocator);
}
