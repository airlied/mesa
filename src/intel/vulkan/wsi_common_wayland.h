#ifndef WSI_COMMON_WAYLAND_H
#define WSI_COMMON_WAYLAND_H

#include "wsi_common.h"

VkBool32
wsi_wl_get_presentation_support(struct wsi_device *wsi_device,
				struct wl_display *wl_display);

VkResult wsi_create_wl_surface(const VkAllocationCallbacks *pAllocator,
			       const VkWaylandSurfaceCreateInfoKHR *pCreateInfo,
			       VkSurfaceKHR *pSurface);
#endif
