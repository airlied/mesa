#include "nouveau_device.h"

#include <drm/drm.h>
#include <nouveau/nouveau.h>
#include <nouveau_drm.h>
#include <nvif/cl0080.h>
#include <nvif/class.h>

#include "util/os_file.h"
#include "util/os_misc.h"

static uint8_t
cls_for_chipset(uint16_t chipset)
{
   if (chipset >= 0x180)
      assert(!"unknown chipset");
   else if (chipset == 0x17b)
      return 0xc7;
   else if (chipset >= 0x172)
      return 0xc6;
   else if (chipset >= 0x170)
      return 0xc5;
   else if (chipset >= 0x160)
      return 0xc4;
   else if (chipset >= 0x140)
      return 0xc3;
   else if (chipset >= 0x132)
      return 0xc1;
   else if (chipset >= 0x130)
      return 0xc0;
   else if (chipset >= 0x120)
      return 0xb1;
   else if (chipset >= 0x110)
      return 0xb0;
   else if (chipset >= 0x0f0)
      return 0xa2;
   else if (chipset >= 0x0ea)
      return 0xa1;
   else if (chipset >= 0x0e0)
      return 0xa0;
   // GF110 is like GF100
   else if (chipset == 0x0c8)
      return 0x90;
   else if (chipset >= 0x0c1)
      return 0x91;
   else if (chipset >= 0x0c0)
      return 0x90;
   else if (chipset >= 0x0a3)
      return 0x85;
   // GT200 is special
   else if (chipset >= 0x0a0)
      return 0x86;
   else if (chipset >= 0x082)
      return 0x82;
   // this has to be == because 0x63 is older than 0x50
   else if (chipset == 0x050)
      return 0x50;
   else if (chipset >= 0x044)
      return 0x44;
   else if (chipset >= 0x040)
      return 0x40;
   else if (chipset >= 0x036)
      return 0x36;
   else if (chipset >= 0x020)
      return 0x20;
   return 0x00;
}

static uint8_t
sm_for_cls(uint8_t cls, uint16_t chipset)
{
   switch (cls) {
   case 0xc7:
      return 0x87;
   case 0xc6:
      return 0x86;
   case 0xc5:
      return 0x80;
   case 0xc4:
      return 0x75;
   case 0xc3:
      return chipset >= 0x14b ? 0x72 : 0x70;
   case 0xc1:
      // TODO maybe that's 0xc2?
      return chipset >= 0x13b ? 0x62 : 0x61;
   case 0xc0:
      return 0x60;
   case 0xb1:
      // TODO is there a 0xb2?
      return chipset >= 0x12b ? 0x53 : 0x52;
   case 0xb0:
      return 0x50;
   case 0xa2:
      return 0x35;
   case 0xa1:
      return 0x31;
   case 0xa0:
      return 0x30;
   case 0x91:
      return 0x21;
   case 0x90:
      return 0x20;
   case 0x86:
      return 0x13;
   case 0x85:
      return 0x12;
   case 0x82:
      return 0x11;
   case 0x50:
      return 0x10;
   default:
      return 0;
   }
}

struct nouveau_ws_device *
nouveau_ws_device_new(int fd)
{
   struct nouveau_ws_device_priv *device = CALLOC_STRUCT(nouveau_ws_device_priv);
   uint64_t device_id = 0;
   struct nouveau_drm *drm;
   struct nouveau_device *dev;
   int dup_fd = os_dupfd_cloexec(fd);

   if (nouveau_drm_new(dup_fd, &drm)) {
      return NULL;
   }

   struct nv_device_v0 v0 = {
      .device = ~0ULL,
   };

   if (nouveau_device_new(&drm->client, NV_DEVICE, &v0, sizeof(v0), &dev)) {
      goto out_drm;
   }

   if (nouveau_getparam(dev, NOUVEAU_GETPARAM_PCI_DEVICE, &device_id)) {
      goto out_dev;
   }

   device->base.vendor_id = 0x10de;
   device->base.device_id = device_id;
   device->base.chipset = dev->chipset;
   device->base.cls = cls_for_chipset(dev->chipset);
   device->base.cm = sm_for_cls(device->base.cls, dev->chipset);
   device->base.vram_size = dev->vram_size;
   os_get_available_system_memory(&device->base.gart_size);
   device->base.gart_size = MIN2(device->base.gart_size, dev->gart_size);
   device->base.is_integrated = dev->vram_size == 0;
   device->drm = drm;
   device->dev = dev;
   device->fd = dup_fd;

   if (dev->vram_size == 0)
      device->local_mem_domain = NOUVEAU_GEM_DOMAIN_GART;
   else
      device->local_mem_domain = NOUVEAU_GEM_DOMAIN_VRAM;

   uint64_t value;
   if (nouveau_getparam(dev, NOUVEAU_GETPARAM_GRAPH_UNITS, &value))
      goto out_dev;
   device->base.gpc_count = value & 0x000000ff;
   device->base.mp_count = value >> 8;
   device->base.mp_count_compute = device->base.mp_count;

   return &device->base;

out_dev:
   nouveau_device_del(&dev);
out_drm:
   nouveau_drm_del(&drm);
   close(dup_fd);
   return NULL;
}

void
nouveau_ws_device_destroy(struct nouveau_ws_device *device)
{
   if (!device)
      return;

   struct nouveau_ws_device_priv *priv = nouveau_ws_device(device);

   nouveau_device_del(&priv->dev);
   nouveau_drm_del(&priv->drm);
   close(priv->fd);

   FREE(priv);
}
