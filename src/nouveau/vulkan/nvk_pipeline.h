#ifndef NVK_PIPELINE_H
#define NVK_PIPELINE_H 1

#include "nvk_shader.h"
#include "vk_object.h"

struct vk_pipeline_cache;

enum nvk_pipeline_type {
   NVK_PIPELINE_GRAPHICS,
   NVK_PIPELINE_COMPUTE,
};

struct nvk_pipeline {
   struct vk_object_base base;

   enum nvk_pipeline_type type;

   struct nvk_shader shaders[MESA_SHADER_STAGES];
};

VK_DEFINE_NONDISP_HANDLE_CASTS(nvk_pipeline, base, VkPipeline,
                               VK_OBJECT_TYPE_PIPELINE)

struct nvk_compute_pipeline {
   struct nvk_pipeline base;

   uint32_t qmd_template[64];
};

VkResult
nvk_compute_pipeline_create(struct nvk_device *device,
                            struct vk_pipeline_cache *cache,
                            const VkComputePipelineCreateInfo *pCreateInfo,
                            const VkAllocationCallbacks *pAllocator,
                            VkPipeline *pPipeline);

#endif
