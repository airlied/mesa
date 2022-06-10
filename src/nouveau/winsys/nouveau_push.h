#ifndef NOUVEAU_CMD_BUF
#define NOUVEAU_CMD_BUF 1

#include "nouveau_private.h"

#include "nouveau_bo.h"
#include "nouveau_drm.h"
#include "util/u_dynarray.h"

struct nouveau_ws_context;
struct nouveau_ws_device;

struct nouveau_ws_push_bo {
   struct nouveau_ws_bo *bo;
   enum nouveau_ws_bo_map_flags flags;
};

struct nouveau_ws_push {
   struct util_dynarray bos;
   struct nouveau_ws_bo *bo;
   uint32_t *orig_map;
   uint32_t *map;
   uint32_t *end;

   uint32_t *last_size;
};

struct nouveau_ws_push *nouveau_ws_push_new(struct nouveau_ws_device *, uint64_t size);
void nouveau_ws_push_destroy(struct nouveau_ws_push *);
int nouveau_ws_push_submit(struct nouveau_ws_push *, struct nouveau_ws_device *, struct nouveau_ws_context *);
void nouveau_ws_push_ref(struct nouveau_ws_push *, struct nouveau_ws_bo *, enum nouveau_ws_bo_map_flags);
void nouveau_ws_push_reset(struct nouveau_ws_push *);

#define SUBC_NVA0C0 1
#define SUBC_NVC0C0 1
#define SUBC_NVC3C0 1

#define SUBC_NV902D 3

#define SUBC_NV90B5 4
#define SUBC_NVC1B5 4

static inline uint32_t
NVC0_FIFO_PKHDR_SQ(int subc, int mthd, unsigned size)
{
   return 0x20000000 | (size << 16) | (subc << 13) | (mthd >> 2);
}

static inline void
__push_mthd(struct nouveau_ws_push *push, int subc, uint32_t mthd)
{
   push->last_size = push->map;
   *push->map = NVC0_FIFO_PKHDR_SQ(subc, mthd, 0);
   push->map++;
}

#define P_MTHD(push, class, mthd) __push_mthd(push, SUBC_##class, class##_##mthd)

static inline uint32_t
NVC0_FIFO_PKHDR_IL(int subc, int mthd, uint16_t data)
{
   assert(data < 0x2000);
   return 0x80000000 | (data << 16) | (subc << 13) | (mthd >> 2);
}

static inline void
__push_immd(struct nouveau_ws_push *push, int subc, uint32_t mthd, uint32_t val)
{
   push->last_size = push->map;
   *push->map = NVC0_FIFO_PKHDR_IL(subc, mthd, val);
   push->map++;
}

#define P_IMMD(push, class, mthd, args...) do {\
   uint32_t __val; \
   V_##class##_##mthd(__val, args);         \
   __push_immd(push, SUBC_##class, class##_##mthd, __val); \
   } while(0)

static inline uint32_t
NVC0_FIFO_PKHDR_1I(int subc, int mthd, unsigned size)
{
   return 0xa0000000 | (size << 16) | (subc << 13) | (mthd >> 2);
}

static inline void
__push_1inc(struct nouveau_ws_push *push, int subc, uint32_t mthd)
{
   push->last_size = push->map;
   *push->map = NVC0_FIFO_PKHDR_1I(subc, mthd, 0);
   push->map++;
}

#define P_1INC(push, class, mthd) __push_1inc(push, SUBC_##class, class##_##mthd)

static inline uint32_t
NVC0_FIFO_PKHDR_0I(int subc, int mthd, unsigned size)
{
   return 0x60000000 | (size << 16) | (subc << 13) | (mthd >> 2);
}

static inline void
__push_0inc(struct nouveau_ws_push *push, int subc, uint32_t mthd)
{
   push->last_size = push->map;
   *push->map = NVC0_FIFO_PKHDR_0I(subc, mthd, 0);
   push->map++;
}

#define P_0INC(push, class, mthd) __push_0inc(push, SUBC_##class, class##_##mthd)

static inline bool
nvk_push_update_count(struct nouveau_ws_push *push, uint16_t count)
{
   uint32_t last_hdr_val = *push->last_size;

   assert(count <= 0x1fff);
   if (count > 0x1fff)
      return false;

   /* size is encoded at 28:16 */
   uint32_t new_count = (count + (last_hdr_val >> 16)) & 0x1fff;
   bool overflow = new_count < count;
   /* if we would overflow, don't change anything and just let it be */
   assert(!overflow);
   if (overflow)
      return false;

   last_hdr_val &= ~0x1fff0000;
   last_hdr_val |= new_count << 16;
   *push->last_size = last_hdr_val;
   return true;
}

static inline void
P_INLINE_DATA(struct nouveau_ws_push *push, uint32_t value)
{
   if (nvk_push_update_count(push, 1)) {
      /* push new value */
      *push->map = value;
      push->map++;
   }
}

static inline void
P_INLINE_ARRAY(struct nouveau_ws_push *push, const uint32_t *data, int num_dw)
{
   if (nvk_push_update_count(push, num_dw)) {
      /* push new value */
      memcpy(push->map, data, num_dw * 4);
      push->map += num_dw;
   }
}

/* internally used by generated inlines. */
static inline void
nvk_push_val(struct nouveau_ws_push *push, uint32_t idx, uint32_t val)
{
   UNUSED uint32_t last_hdr_val = *push->last_size;
   UNUSED bool is_1inc = (last_hdr_val & 0xe0000000) == 0xa0000000;
   UNUSED bool is_immd = (last_hdr_val & 0xe0000000) == 0x80000000;
   UNUSED uint16_t last_method = (last_hdr_val & 0x1fff) << 2;

   uint16_t distance = push->map - push->last_size - 1;
   if (is_1inc)
      distance = MIN2(1, distance);
   last_method += distance * 4;

   /* can't have empty headers ever */
   assert(last_hdr_val);
   assert(!is_immd);
   assert(last_method == idx);

   P_INLINE_DATA(push, val);
}
#endif
