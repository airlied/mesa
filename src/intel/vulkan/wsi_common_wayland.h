#ifndef WSI_COMMON_WAYLAND_H
#define WSI_COMMON_WAYLAND_H

VkBool32
wsi_wl_get_presentation_support(struct anv_wsi_device *wsi_device,
				struct wl_display *wl_display);

VkResult anv_create_wl_surface(const VkAllocationCallbacks *pAllocator,
			       const VkWaylandSurfaceCreateInfoKHR *pCreateInfo,
			       VkSurfaceKHR *pSurface);
#endif
