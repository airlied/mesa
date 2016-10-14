#ifndef WSI_COMMON_X11_H
#define WSI_COMMON_X11_H

VkBool32 anv_get_physical_device_xcb_presentation_support(
    struct anv_wsi_device *wsi_device,
    VkAllocationCallbacks *alloc,
    uint32_t                                    queueFamilyIndex,
    xcb_connection_t*                           connection,
    xcb_visualid_t                              visual_id);

VkResult anv_create_xcb_surface(const VkAllocationCallbacks *pAllocator,
				const VkXcbSurfaceCreateInfoKHR *pCreateInfo,
				VkSurfaceKHR *pSurface);

VkResult anv_create_xlib_surface(const VkAllocationCallbacks *pAllocator,
				 const VkXlibSurfaceCreateInfoKHR *pCreateInfo,
				 VkSurfaceKHR *pSurface);
#endif
