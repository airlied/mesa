#pragma once

#include <amdgpu.h>

#include "radv_radeon_winsys.h"
struct radv_device_info {

   uint32_t pci_id;
  //   enum chip_family family;
  enum chip_class chip_class;

};
