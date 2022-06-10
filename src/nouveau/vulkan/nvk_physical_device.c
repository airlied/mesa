#include "nvk_physical_device.h"

#include "nvk_bo_sync.h"
#include "nvk_entrypoints.h"
#include "nvk_format.h"
#include "nvk_instance.h"
#include "nvk_wsi.h"

#include "vulkan/runtime/vk_device.h"
#include "vulkan/wsi/wsi_common.h"

VKAPI_ATTR void VKAPI_CALL
nvk_GetPhysicalDeviceFeatures2(VkPhysicalDevice physicalDevice,
   VkPhysicalDeviceFeatures2 *pFeatures)
{
   // VK_FROM_HANDLE(nvk_physical_device, pdevice, physicalDevice);

   pFeatures->features = (VkPhysicalDeviceFeatures) {
      .robustBufferAccess = true,
      /* More features */
   };

   VkPhysicalDeviceVulkan11Features core_1_1 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
      /* Vulkan 1.1 features */
   };

   VkPhysicalDeviceVulkan12Features core_1_2 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
      /* Vulkan 1.2 features */
   };

   VkPhysicalDeviceVulkan13Features core_1_3 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
      /* Vulkan 1.3 features */
   };

   vk_foreach_struct(ext, pFeatures->pNext)
   {
      if (vk_get_physical_device_core_1_1_feature_ext(ext, &core_1_1))
         continue;
      if (vk_get_physical_device_core_1_2_feature_ext(ext, &core_1_2))
         continue;
      if (vk_get_physical_device_core_1_3_feature_ext(ext, &core_1_3))
         continue;

      switch (ext->sType) {
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_4444_FORMATS_FEATURES_EXT: {
         VkPhysicalDevice4444FormatsFeaturesEXT *features = (void *)ext;
         features->formatA4R4G4B4 = true;
         features->formatA4B4G4R4 = true;
         break;
      }
      /* More feature structs */
      default:
         break;
      }
   }
}

VKAPI_ATTR void VKAPI_CALL
nvk_GetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice,
   VkPhysicalDeviceProperties2 *pProperties)
{
   VK_FROM_HANDLE(nvk_physical_device, pdevice, physicalDevice);

   pProperties->properties = (VkPhysicalDeviceProperties) {
      .apiVersion = VK_MAKE_VERSION(1, 0, VK_HEADER_VERSION),
      .driverVersion = vk_get_driver_version(),
      .vendorID = pdevice->dev->vendor_id,
      .deviceID = pdevice->dev->device_id,
      .deviceType = pdevice->dev->is_integrated ? VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU
                                                : VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU,
      .limits = (VkPhysicalDeviceLimits) {
         .maxImageArrayLayers = 2048,
         .maxImageDimension1D = pdevice->dev->chipset >= 0x130 ? 0x8000 : 0x4000,
         .maxImageDimension2D = pdevice->dev->chipset >= 0x130 ? 0x8000 : 0x4000,
         .nonCoherentAtomSize = 64,
      },

      /* More properties */
   };

   VkPhysicalDeviceVulkan11Properties core_1_1 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES,
      /* Vulkan 1.1 properties */
   };

   VkPhysicalDeviceVulkan12Properties core_1_2 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES,
      /* Vulkan 1.2 properties */
   };

   VkPhysicalDeviceVulkan13Properties core_1_3 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES,
      /* Vulkan 1.3 properties */
   };

   vk_foreach_struct(ext, pProperties->pNext)
   {
      if (vk_get_physical_device_core_1_1_property_ext(ext, &core_1_1))
         continue;
      if (vk_get_physical_device_core_1_2_property_ext(ext, &core_1_2))
         continue;
      if (vk_get_physical_device_core_1_3_property_ext(ext, &core_1_3))
         continue;

      switch (ext->sType) {
      /* More property structs */
      default:
         break;
      }
   }
}

PUBLIC VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetPhysicalDeviceProcAddr(VkInstance _instance, const char *pName)
{
   VK_FROM_HANDLE(nvk_instance, instance, _instance);
   return vk_instance_get_physical_device_proc_addr(&instance->vk, pName);
}

static void
nvk_get_device_extensions(const struct nvk_physical_device *device,
   struct vk_device_extension_table *ext)
{
   *ext = (struct vk_device_extension_table) {
      .KHR_copy_commands2 = true,
#ifdef NVK_USE_WSI_PLATFORM
      .KHR_swapchain = true,
      .KHR_swapchain_mutable_format = true,
#endif
      .KHR_variable_pointers = true,
   };
}

static VkResult
nvk_physical_device_try_create(struct nvk_instance *instance,
   drmDevicePtr drm_device,
   struct nvk_physical_device **device_out)
{
   // const char *primary_path = drm_device->nodes[DRM_NODE_PRIMARY];
   const char *path = drm_device->nodes[DRM_NODE_RENDER];
   VkResult result;
   int fd;

   fd = open(path, O_RDWR | O_CLOEXEC);
   if (fd < 0) {
      if (errno == ENOMEM) {
         return vk_errorf(
            instance, VK_ERROR_OUT_OF_HOST_MEMORY, "Unable to open device %s: out of memory", path);
      }
      return vk_errorf(
         instance, VK_ERROR_INCOMPATIBLE_DRIVER, "Unable to open device %s: %m", path);
   }

   struct nouveau_ws_device *ndev = nouveau_ws_device_new(fd);
   if (!ndev) {
      result = vk_error(instance, VK_ERROR_INCOMPATIBLE_DRIVER);
      goto fail_fd;
   }

   vk_warn_non_conformant_implementation("nvk");

   struct nvk_physical_device *device =
      vk_zalloc(&instance->vk.alloc, sizeof(*device), 8, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);

   if (device == NULL) {
      result = vk_error(instance, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto fail_dev_alloc;
   }

   struct vk_physical_device_dispatch_table dispatch_table;
   vk_physical_device_dispatch_table_from_entrypoints(
      &dispatch_table, &nvk_physical_device_entrypoints, true);
   vk_physical_device_dispatch_table_from_entrypoints(
      &dispatch_table, &wsi_physical_device_entrypoints, false);

   struct vk_device_extension_table supported_extensions;
   nvk_get_device_extensions(device, &supported_extensions);

   result =
      vk_physical_device_init(&device->vk, &instance->vk, &supported_extensions, &dispatch_table);

   if (result != VK_SUCCESS) {
      vk_error(instance, result);
      goto fail_alloc;
   }

   device->instance = instance;
   device->dev = ndev;

   result = nvk_init_wsi(device);
   if (result != VK_SUCCESS) {
      vk_error(instance, result);
      goto fail_alloc;
   }

   device->mem_heaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
   device->mem_types[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
   device->mem_types[0].heapIndex = 0;

   if (ndev->vram_size) {
      device->mem_type_cnt = 2;
      device->mem_heap_cnt = 2;

      device->mem_heaps[0].size = ndev->vram_size;
      device->mem_heaps[1].size = ndev->gart_size;
      device->mem_heaps[1].flags = 0;
      device->mem_types[1].heapIndex = 1;
      device->mem_types[1].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
   } else {
      device->mem_type_cnt = 1;
      device->mem_heap_cnt = 1;

      device->mem_heaps[0].size = ndev->gart_size;
      device->mem_types[0].propertyFlags |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
   }

   unsigned st_idx = 0;
   device->sync_types[st_idx++] = &nvk_bo_sync_type;
   device->sync_types[st_idx++] = NULL;
   assert(st_idx <= ARRAY_SIZE(device->sync_types));
   device->vk.supported_sync_types = device->sync_types;

   *device_out = device;

   close(fd);
   return VK_SUCCESS;

fail_alloc:
   vk_free(&instance->vk.alloc, device);

fail_dev_alloc:
   nouveau_ws_device_destroy(ndev);
   return result;

fail_fd:
   close(fd);
   return result;
}

void
nvk_physical_device_destroy(struct nvk_physical_device *device)
{
   nvk_finish_wsi(device);
   nouveau_ws_device_destroy(device->dev);
   vk_physical_device_finish(&device->vk);
   vk_free(&device->instance->vk.alloc, device);
}

static VkResult
nvk_enumerate_physical_devices(struct nvk_instance *instance)
{
   if (instance->physical_devices_enumerated)
      return VK_SUCCESS;

   instance->physical_devices_enumerated = true;

   int max_devices = drmGetDevices2(0, NULL, 0);
   if (max_devices < 1)
      return VK_SUCCESS;

   drmDevicePtr *devices = MALLOC(max_devices * sizeof(drmDevicePtr));
   drmGetDevices2(0, devices, max_devices);

   VkResult result = VK_SUCCESS;
   for (unsigned i = 0; i < (unsigned)max_devices; i++) {
      if (devices[i]->available_nodes & 1 << DRM_NODE_RENDER &&
         devices[i]->bustype == DRM_BUS_PCI && devices[i]->deviceinfo.pci->vendor_id == 0x10de) {
         struct nvk_physical_device *pdevice;
         result = nvk_physical_device_try_create(instance, devices[i], &pdevice);
         /* Incompatible DRM device, skip. */
         if (result == VK_ERROR_INCOMPATIBLE_DRIVER) {
            result = VK_SUCCESS;
            continue;
         }

         /* Error creating the physical device, report the error. */
         if (result != VK_SUCCESS)
            break;

         list_addtail(&pdevice->link, &instance->physical_devices);
      }
   }
   drmFreeDevices(devices, max_devices);
   FREE(devices);

   /* If we successfully enumerated any devices, call it success */
   return result;
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_EnumeratePhysicalDevices(VkInstance _instance,
   uint32_t *pPhysicalDeviceCount,
   VkPhysicalDevice *pPhysicalDevices)
{
   VK_FROM_HANDLE(nvk_instance, instance, _instance);
   VK_OUTARRAY_MAKE_TYPED(VkPhysicalDevice, out, pPhysicalDevices, pPhysicalDeviceCount);

   VkResult result = nvk_enumerate_physical_devices(instance);
   if (result != VK_SUCCESS)
      return result;

   list_for_each_entry(struct nvk_physical_device, pdevice, &instance->physical_devices, link)
   {
      vk_outarray_append_typed(VkPhysicalDevice, &out, i)
      {
         *i = nvk_physical_device_to_handle(pdevice);
      }
   }

   return vk_outarray_status(&out);
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_EnumeratePhysicalDeviceGroups(VkInstance _instance,
   uint32_t *pPhysicalDeviceGroupCount,
   VkPhysicalDeviceGroupProperties *pPhysicalDeviceGroupProperties)
{
   VK_FROM_HANDLE(nvk_instance, instance, _instance);
   VK_OUTARRAY_MAKE_TYPED(VkPhysicalDeviceGroupProperties,
      out,
      pPhysicalDeviceGroupProperties,
      pPhysicalDeviceGroupCount);

   VkResult result = nvk_enumerate_physical_devices(instance);
   if (result != VK_SUCCESS)
      return result;

   list_for_each_entry(struct nvk_physical_device, pdevice, &instance->physical_devices, link)
   {
      vk_outarray_append_typed(VkPhysicalDeviceGroupProperties, &out, p)
      {
         p->physicalDeviceCount = 1;
         memset(p->physicalDevices, 0, sizeof(p->physicalDevices));
         p->physicalDevices[0] = nvk_physical_device_to_handle(pdevice);
         p->subsetAllocation = false;
      }
   }

   return vk_outarray_status(&out);
}

VKAPI_ATTR void VKAPI_CALL
nvk_GetPhysicalDeviceMemoryProperties2(VkPhysicalDevice physicalDevice,
   VkPhysicalDeviceMemoryProperties2 *pMemoryProperties)
{
   VK_FROM_HANDLE(nvk_physical_device, pdevice, physicalDevice);

   pMemoryProperties->memoryProperties.memoryHeapCount = pdevice->mem_heap_cnt;
   for (int i = 0; i < pdevice->mem_heap_cnt; i++) {
      pMemoryProperties->memoryProperties.memoryHeaps[i] = pdevice->mem_heaps[i];
   }

   pMemoryProperties->memoryProperties.memoryTypeCount = pdevice->mem_type_cnt;
   for (int i = 0; i < pdevice->mem_type_cnt; i++) {
      pMemoryProperties->memoryProperties.memoryTypes[i] = pdevice->mem_types[i];
   }

   vk_foreach_struct(ext, pMemoryProperties->pNext)
   {
      switch (ext->sType) {
      default:
         nvk_debug_ignored_stype(ext->sType);
         break;
      }
   }
}

VKAPI_ATTR void VKAPI_CALL
nvk_GetPhysicalDeviceQueueFamilyProperties2(VkPhysicalDevice physicalDevice,
   uint32_t *pQueueFamilyPropertyCount,
   VkQueueFamilyProperties2 *pQueueFamilyProperties)
{
   // VK_FROM_HANDLE(nvk_physical_device, pdevice, physicalDevice);
   VK_OUTARRAY_MAKE_TYPED(
      VkQueueFamilyProperties2, out, pQueueFamilyProperties, pQueueFamilyPropertyCount);

   vk_outarray_append_typed(VkQueueFamilyProperties2, &out, p) {
      p->queueFamilyProperties.queueFlags = VK_QUEUE_GRAPHICS_BIT |
                                            VK_QUEUE_COMPUTE_BIT |
                                            VK_QUEUE_TRANSFER_BIT;
      p->queueFamilyProperties.queueCount = 1;
      p->queueFamilyProperties.timestampValidBits = 64;
      p->queueFamilyProperties.minImageTransferGranularity = (VkExtent3D){1, 1, 1};
   }
}

static void
nvk_physical_device_get_format_properties(struct nvk_physical_device *physical_device,
                                          VkFormat format, VkFormatProperties3 *out_properties)
{
   VkFormatFeatureFlags2 linear = 0, tiled = 0, buffer = 0;

   if (format == VK_FORMAT_B8G8R8A8_UNORM) {
      buffer |= VK_FORMAT_FEATURE_2_STORAGE_TEXEL_BUFFER_BIT |
                VK_FORMAT_FEATURE_2_STORAGE_READ_WITHOUT_FORMAT_BIT |
                VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT;
   }

   out_properties->linearTilingFeatures = linear;
   out_properties->optimalTilingFeatures = tiled;
   out_properties->bufferFeatures = buffer;
}

static VkFormatFeatureFlags
features2_to_features(VkFormatFeatureFlags2 features2)
{
   return features2 & VK_ALL_FORMAT_FEATURE_FLAG_BITS;
}

VKAPI_ATTR void VKAPI_CALL
nvk_GetPhysicalDeviceFormatProperties2(VkPhysicalDevice physicalDevice,
   VkFormat format,
   VkFormatProperties2 *pFormatProperties)
{
   VK_FROM_HANDLE(nvk_physical_device, physical_device, physicalDevice);
   VkFormatProperties3 format_props;

   nvk_physical_device_get_format_properties(physical_device, format, &format_props);
   pFormatProperties->formatProperties.linearTilingFeatures =
      features2_to_features(format_props.linearTilingFeatures);
   pFormatProperties->formatProperties.optimalTilingFeatures =
      features2_to_features(format_props.optimalTilingFeatures);
   pFormatProperties->formatProperties.bufferFeatures =
      features2_to_features(format_props.bufferFeatures);
   vk_foreach_struct(ext, pFormatProperties->pNext)
   {
      /* Use unsigned since some cases are not in the VkStructureType enum. */
      switch ((unsigned)ext->sType) {
      default:
         nvk_debug_ignored_stype(ext->sType);
         break;
      }
   }
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_GetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice physicalDevice,
   const VkPhysicalDeviceImageFormatInfo2 *base_info,
   VkImageFormatProperties2 *base_props)
{
   if (base_info->usage & ~(VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
      return VK_ERROR_FORMAT_NOT_SUPPORTED;

   for (unsigned i = 0; i < ARRAY_SIZE(nvk_formats); i++) {
      struct nvk_format *format = &nvk_formats[i];

      if (format->vk_format != base_info->format)
         continue;

      if (!format->supports_2d_blit)
         return VK_ERROR_FORMAT_NOT_SUPPORTED;

      if (base_info->type == VK_IMAGE_TYPE_1D)
         base_props->imageFormatProperties.maxExtent = (VkExtent3D){32768, 1, 1};
      else if (base_info->type == VK_IMAGE_TYPE_2D)
         base_props->imageFormatProperties.maxExtent = (VkExtent3D){32768, 32768, 1};
      else
         return VK_ERROR_FORMAT_NOT_SUPPORTED;

      base_props->imageFormatProperties.maxMipLevels = 1;
      base_props->imageFormatProperties.maxArrayLayers = 2048;
      base_props->imageFormatProperties.sampleCounts = 0;
      base_props->imageFormatProperties.maxResourceSize = 0xffffffff; // TODO proper value

      vk_foreach_struct(s, base_props->pNext) {
         switch (s->sType) {
         default:
            nvk_debug_ignored_stype(s->sType);
            break;
         }
      }

      vk_foreach_struct(ext, base_info->pNext)
      {
         /* Use unsigned since some cases are not in the VkStructureType enum. */
         switch ((unsigned)ext->sType) {
         default:
            nvk_debug_ignored_stype(ext->sType);
            break;
         }
      }

      return VK_SUCCESS;
   }

   return VK_ERROR_FORMAT_NOT_SUPPORTED;
}
