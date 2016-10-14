#ifndef WSI_COMMON_H
#define WSI_COMMON_H

#include <stdint.h>
#include <stdbool.h>

#include "vulkan/common/vk_alloc.h"
#include <vulkan/vulkan.h>
#include <vulkan/vk_icd.h>

struct wsi_device;
struct wsi_image_fns {
   VkResult (*create_wsi_image)(VkDevice device_h,
                                const VkSwapchainCreateInfoKHR *pCreateInfo,
                                const VkAllocationCallbacks *pAllocator,
                                VkImage *image_p,
                                VkDeviceMemory *memory_p,
                                uint32_t *size_p,
                                uint32_t *offset_p,
                                uint32_t *row_pitch_p,
                                int *fd_p);
   void (*free_wsi_image)(VkDevice device,
                          const VkAllocationCallbacks *pAllocator,
                          VkImage image_h,
                          VkDeviceMemory memory_h);
};

struct wsi_swapchain {

   VkDevice device;
   VkAllocationCallbacks alloc;
   const struct wsi_image_fns *image_fns;
   VkFence fences[3];

   VkResult (*destroy)(struct wsi_swapchain *swapchain,
                       const VkAllocationCallbacks *pAllocator);
   VkResult (*get_images)(struct wsi_swapchain *swapchain,
                          uint32_t *pCount, VkImage *pSwapchainImages);
   VkResult (*acquire_next_image)(struct wsi_swapchain *swap_chain,
                                  uint64_t timeout, VkSemaphore semaphore,
                                  uint32_t *image_index);
   VkResult (*queue_present)(struct wsi_swapchain *swap_chain,
                             uint32_t image_index);
};

struct wsi_interface {
   VkResult (*get_support)(VkIcdSurfaceBase *surface,
                           struct wsi_device *wsi_device,
                           const VkAllocationCallbacks *alloc,
                           uint32_t queueFamilyIndex,
                           VkBool32* pSupported);
   VkResult (*get_capabilities)(VkIcdSurfaceBase *surface,
                                VkSurfaceCapabilitiesKHR* pSurfaceCapabilities);
   VkResult (*get_formats)(VkIcdSurfaceBase *surface,
                           struct wsi_device *wsi_device,
                           uint32_t* pSurfaceFormatCount,
                           VkSurfaceFormatKHR* pSurfaceFormats);
   VkResult (*get_present_modes)(VkIcdSurfaceBase *surface,
                                 uint32_t* pPresentModeCount,
                                 VkPresentModeKHR* pPresentModes);
   VkResult (*create_swapchain)(VkIcdSurfaceBase *surface,
                                VkDevice device,
                                struct wsi_device *wsi_device,
                                const VkSwapchainCreateInfoKHR* pCreateInfo,
                                const VkAllocationCallbacks* pAllocator,
                                const struct wsi_image_fns *image_fns,
                                struct wsi_swapchain **swapchain);
};

#define VK_ICD_WSI_PLATFORM_MAX 5

struct wsi_device {
    struct wsi_interface *                  wsi[VK_ICD_WSI_PLATFORM_MAX];
};

struct wsi_callbacks {
   void (*get_phys_device_format_properties)(VkPhysicalDevice physicalDevice,
                                             VkFormat format,
                                             VkFormatProperties *pFormatProperties);
};

#define WSI_DEFINE_NONDISP_HANDLE_CASTS(__wsi_type, __VkType)              \
                                                                           \
   static inline struct __wsi_type *                                       \
   __wsi_type ## _from_handle(__VkType _handle)                            \
   {                                                                       \
      return (struct __wsi_type *)(uintptr_t) _handle;                     \
   }                                                                       \
                                                                           \
   static inline __VkType                                                  \
   __wsi_type ## _to_handle(struct __wsi_type *_obj)                       \
   {                                                                       \
      return (__VkType)(uintptr_t) _obj;                                   \
   }

WSI_DEFINE_NONDISP_HANDLE_CASTS(_VkIcdSurfaceBase, VkSurfaceKHR)
WSI_DEFINE_NONDISP_HANDLE_CASTS(wsi_swapchain, VkSwapchainKHR)
VkResult wsi_wl_init_wsi(struct wsi_device *wsi_device,
                         const VkAllocationCallbacks *alloc,
                         VkPhysicalDevice physical_device,
                         const struct wsi_callbacks *cbs);
void wsi_wl_finish_wsi(struct wsi_device *wsi_device,
                       const VkAllocationCallbacks *alloc);

VkResult wsi_x11_init_wsi(struct wsi_device *wsi_device,
                          const VkAllocationCallbacks *alloc);
void wsi_x11_finish_wsi(struct wsi_device *wsi_device,
                        const VkAllocationCallbacks *alloc);

#endif
