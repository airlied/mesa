#ifndef NOUVEAU_DEVICE
#define NOUVEAU_DEVICE 1

#include "nouveau_private.h"

#include <stddef.h>

struct nouveau_ws_device {
   uint16_t vendor_id;
   uint16_t device_id;
   uint32_t chipset;
   /* first byte of class id */
   uint8_t cls;
   /* maps to CUDAs Compute capability version */
   uint8_t cm;
   uint64_t vram_size;
   uint64_t gart_size;
   bool is_integrated;

   uint8_t gpc_count;
   uint16_t mp_count;
   uint16_t mp_count_compute; /* magic reg can make compute use fewer MPs */
};

/* don't use directly, gets removed once the new UAPI is here */
struct nouveau_ws_device_priv {
   struct nouveau_ws_device base;
   struct nouveau_drm *drm;
   struct nouveau_device *dev;
   int fd;
   uint32_t local_mem_domain;
};

static struct nouveau_ws_device_priv *
nouveau_ws_device(struct nouveau_ws_device *dev)
{
   return container_of(dev, struct nouveau_ws_device_priv, base);
}

struct nouveau_ws_device *nouveau_ws_device_new(int fd);
void nouveau_ws_device_destroy(struct nouveau_ws_device *);

#endif
