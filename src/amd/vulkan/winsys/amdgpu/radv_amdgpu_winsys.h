
#pragma once

#include "radv_radeon_winsys.h"
#include "addrlib/addrinterface.h"
#include <amdgpu.h>

struct amdgpu_winsys {
  struct radeon_winsys base;
  amdgpu_device_handle dev;

  struct radeon_info info;
  struct amdgpu_gpu_info amdinfo;
  ADDR_HANDLE addrlib;

  uint32_t rev_id;
  unsigned family;
};

static inline struct amdgpu_winsys *
amdgpu_winsys(struct radeon_winsys *base)
{
   return (struct amdgpu_winsys*)base;
}
