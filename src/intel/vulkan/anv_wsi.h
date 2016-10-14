/*
 * Copyright © 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifndef ANV_WSI_H
#define ANV_WSI_H

#include "anv_private.h"

struct anv_swapchain;

ANV_DEFINE_NONDISP_HANDLE_CASTS(anv_swapchain, VkSwapchainKHR)

struct anv_wsi_callbacks {
   void (*get_phys_device_format_properties)(VkPhysicalDevice physicalDevice,
                                             VkFormat format,
                                             VkFormatProperties *pFormatProperties);
};

VkResult anv_x11_init_wsi(struct anv_wsi_device *wsi_device,
                          const VkAllocationCallbacks *alloc);
void anv_x11_finish_wsi(struct anv_wsi_device *wsi_device,
                        const VkAllocationCallbacks *alloc);

VkResult anv_wl_init_wsi(struct anv_wsi_device *wsi_device,
                         const VkAllocationCallbacks *alloc,
                         VkPhysicalDevice physical_device,
                         const struct anv_wsi_callbacks *cbs);
void anv_wl_finish_wsi(struct anv_wsi_device *wsi_device,
                       const VkAllocationCallbacks *alloc);
#endif /* ANV_WSI_H */
