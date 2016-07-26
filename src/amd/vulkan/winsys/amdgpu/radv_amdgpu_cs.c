/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
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

#include <stdlib.h>
#include <amdgpu.h>
#include <amdgpu_drm.h>
#include <assert.h>

#include "radv_radeon_winsys.h"
#include "radv_amdgpu_cs.h"
#include "radv_amdgpu_bo.h"
#include "sid.h"

struct amdgpu_cs {
	struct radeon_winsys_cs base;
	struct amdgpu_winsys *ws;

	struct amdgpu_cs_request    request;
	struct amdgpu_cs_ib_info    ib;

	struct radeon_winsys_bo     *ib_buffer;
	uint8_t                 *ib_mapped;
	unsigned                    max_num_buffers;
	unsigned                    num_buffers;
	amdgpu_bo_handle            *handles;
	uint8_t                     *priorities;
	//  struct amdgpu_cs_buffer     *buffers;

	struct radeon_winsys_bo     **old_ib_buffers;
	unsigned                    num_old_ib_buffers;
	unsigned                    max_num_old_ib_buffers;
	unsigned                    *ib_size_ptr;
	bool                        failed;
};

static inline struct amdgpu_cs *
amdgpu_cs(struct radeon_winsys_cs *base)
{
	return (struct amdgpu_cs*)base;
}


static struct radeon_winsys_fence *amdgpu_create_fence()
{
	struct amdgpu_cs_fence *fence = calloc(1, sizeof(struct amdgpu_cs_fence));
	return (struct radeon_winsys_fence*)fence;
}

static void amdgpu_destroy_fence(struct radeon_winsys_fence *_fence)
{
	struct amdgpu_cs_fence *fence = (struct amdgpu_cs_fence *)_fence;
	free(fence);
}

static bool amdgpu_fence_wait(struct radeon_winsys *_ws,
			      struct radeon_winsys_fence *_fence,
			      uint64_t timeout)
{
	struct amdgpu_winsys *ws = amdgpu_winsys(_ws);
	struct amdgpu_cs_fence *fence = (struct amdgpu_cs_fence *)_fence;
	int r;
	uint32_t expired = 0;
	/* Now use the libdrm query. */
	r = amdgpu_cs_query_fence_status(fence,
					 timeout,
					 AMDGPU_QUERY_FENCE_TIMEOUT_IS_ABSOLUTE,
					 &expired);

	if (r) {
		fprintf(stderr, "amdgpu: amdgpu_cs_query_fence_status failed.\n");
		return false;
	}

	if (expired) {
		return true;
	}
	return false;

}

static void amdgpu_cs_destroy(struct radeon_winsys_cs *rcs)
{
	struct amdgpu_cs *cs = amdgpu_cs(rcs);
	cs->ws->base.buffer_destroy(cs->ib_buffer);
	for (unsigned i = 0; i < cs->num_old_ib_buffers; ++i)
		cs->ws->base.buffer_destroy(cs->old_ib_buffers[i]);
	free(cs->old_ib_buffers);
	free(cs->handles);
	free(cs->priorities);
	free(cs);
}

static boolean amdgpu_init_cs(struct amdgpu_cs *cs,
			      enum ring_type ring_type)
{
	switch (ring_type) {
	case RING_DMA:
		cs->request.ip_type = AMDGPU_HW_IP_DMA;
		break;

	case RING_UVD:
		cs->request.ip_type = AMDGPU_HW_IP_UVD;
		break;

	case RING_VCE:
		cs->request.ip_type = AMDGPU_HW_IP_VCE;
		break;

	case RING_COMPUTE:
		cs->request.ip_type = AMDGPU_HW_IP_COMPUTE;
		break;

	default:
	case RING_GFX:
		cs->request.ip_type = AMDGPU_HW_IP_GFX;
		break;
	}
	cs->request.number_of_ibs = 1;
	cs->request.ibs = &cs->ib;
	return true;
}

static struct radeon_winsys_cs *
amdgpu_cs_create(struct radeon_winsys *ws,
		 enum ring_type ring_type)
{
	struct amdgpu_cs *cs;
	uint32_t ib_size = 20 * 1024 * 4;
	int r;
	cs = calloc(1, sizeof(struct amdgpu_cs));
	if (!cs)
		return NULL;

	cs->ws = amdgpu_winsys(ws);

	cs->ib_buffer = ws->buffer_create(ws, ib_size, 0,
					  RADEON_DOMAIN_GTT,
					  RADEON_FLAG_CPU_ACCESS);
	if (!cs->ib_buffer)
		return NULL;

	amdgpu_init_cs(cs, RING_GFX);
	cs->ib_mapped = ws->buffer_map(cs->ib_buffer);
	if (!cs->ib_mapped) {
		ws->buffer_destroy(cs->ib_buffer);
		free(cs);
		return NULL;
	}

	cs->ib.ib_mc_address = amdgpu_winsys_bo(cs->ib_buffer)->va;
	cs->base.buf = (uint32_t *)cs->ib_mapped;
	cs->base.max_dw = ib_size / 4 - 4;

	ws->cs_add_buffer(&cs->base, cs->ib_buffer, 8);
	return &cs->base;
}

static void amdgpu_cs_grow(struct radeon_winsys_cs *_cs, size_t min_size)
{
	struct amdgpu_cs *cs = amdgpu_cs(_cs);
	uint64_t ib_size = MAX2(min_size * 4 + 16, cs->base.max_dw * 4 * 2);

	if (cs->failed) {
		cs->base.cdw = 0;
		return;
	}

	if (cs->ib_size_ptr)
		*cs->ib_size_ptr = cs->base.cdw + 4;
	else
		cs->ib.size = cs->base.cdw + 4;

	if (cs->num_old_ib_buffers == cs->max_num_old_ib_buffers) {
		cs->max_num_old_ib_buffers = MAX2(1, cs->max_num_old_ib_buffers * 2);
		cs->old_ib_buffers = realloc(cs->old_ib_buffers,
					     cs->max_num_old_ib_buffers * sizeof(void*));
	}

	cs->old_ib_buffers[cs->num_old_ib_buffers++] = cs->ib_buffer;

	cs->ib_buffer = cs->ws->base.buffer_create(&cs->ws->base, ib_size, 0,
						   RADEON_DOMAIN_GTT,
						   RADEON_FLAG_CPU_ACCESS);

	if (!cs->ib_buffer) {
		cs->base.cdw = 0;
		cs->failed = true;
		cs->ib_buffer = cs->old_ib_buffers[--cs->num_old_ib_buffers];
	}

	cs->ib_mapped = cs->ws->base.buffer_map(cs->ib_buffer);
	if (!cs->ib_mapped) {
		cs->ws->base.buffer_destroy(cs->ib_buffer);
		cs->base.cdw = 0;
		cs->failed = true;
		cs->ib_buffer = cs->old_ib_buffers[--cs->num_old_ib_buffers];
	}

	cs->ws->base.cs_add_buffer(&cs->base, cs->ib_buffer, 8);

	while (!cs->base.cdw || (cs->base.cdw & 7) != 4)
		cs->base.buf[cs->base.cdw++] = 0xffff1000;

	cs->base.buf[cs->base.cdw++] = PKT3(PKT3_INDIRECT_BUFFER_CIK, 2, 0);
	cs->base.buf[cs->base.cdw++] = amdgpu_winsys_bo(cs->ib_buffer)->va;
	cs->base.buf[cs->base.cdw++] = amdgpu_winsys_bo(cs->ib_buffer)->va >> 32;
	cs->ib_size_ptr = cs->base.buf + cs->base.cdw;
	cs->base.buf[cs->base.cdw++] = 0;

	cs->base.buf = (uint32_t *)cs->ib_mapped;
	cs->base.cdw = 0;
	cs->base.max_dw = ib_size / 4 - 4;

}

static bool amdgpu_cs_finalize(struct radeon_winsys_cs *_cs)
{
	struct amdgpu_cs *cs = amdgpu_cs(_cs);

	while (!cs->base.cdw || (cs->base.cdw & 7) != 0)
		cs->base.buf[cs->base.cdw++] = 0xffff1000;

	if (cs->ib_size_ptr)
		*cs->ib_size_ptr = cs->base.cdw;
	else
		cs->ib.size = cs->base.cdw;

	return !cs->failed;
}

static void amdgpu_cs_reset(struct radeon_winsys_cs *_cs)
{
	struct amdgpu_cs *cs = amdgpu_cs(_cs);
	cs->base.cdw = 0;
	cs->num_buffers = 0;
	cs->ib_size_ptr = NULL;
	cs->failed = false;

	cs->ws->base.cs_add_buffer(&cs->base, cs->ib_buffer, 8);

	for (unsigned i = 0; i < cs->num_old_ib_buffers; ++i)
		cs->ws->base.buffer_destroy(cs->old_ib_buffers[i]);

	cs->num_old_ib_buffers = 0;
	cs->ib.ib_mc_address = amdgpu_winsys_bo(cs->ib_buffer)->va;
}

static void amdgpu_cs_add_buffer(struct radeon_winsys_cs *_cs,
				 struct radeon_winsys_bo *_bo,
				 uint8_t priority)
{
	struct amdgpu_cs *cs = amdgpu_cs(_cs);
	struct amdgpu_winsys_bo *bo = amdgpu_winsys_bo(_bo);

	for (unsigned i = 0; i < cs->num_buffers; ++i) {
		if (cs->handles[i] == bo->bo) {
			cs->priorities[i] = MAX2(cs->priorities[i], priority);
			return;
		}
	}

	if (cs->num_buffers == cs->max_num_buffers) {
		unsigned new_count = MAX2(1, cs->max_num_buffers * 2);
		cs->handles = realloc(cs->handles, new_count * sizeof(amdgpu_bo_handle));
		cs->priorities = realloc(cs->priorities, new_count * sizeof(uint8_t));
		cs->max_num_buffers = new_count;
	}

	cs->handles[cs->num_buffers] = bo->bo;
	cs->priorities[cs->num_buffers] = priority;
	++cs->num_buffers;
}

static int amdgpu_winsys_cs_submit(struct radeon_winsys_ctx *_ctx,
				   struct radeon_winsys_cs *_cs,
				   struct radeon_winsys_fence *_fence)
{
	int r;
	struct amdgpu_cs *cs = amdgpu_cs(_cs);
	struct amdgpu_ctx *ctx = amdgpu_ctx(_ctx);
	struct amdgpu_cs_fence *fence = (struct amdgpu_cs_fence *)_fence;
	amdgpu_bo_list_handle bo_list;

	if (cs->failed)
		abort();

	r = amdgpu_bo_list_create(cs->ws->dev, cs->num_buffers, cs->handles,
				  cs->priorities, &bo_list);
	if (r) {
		fprintf(stderr, "amdgpu: Failed to created the BO list for submission\n");
		return r;
	}

	cs->request.resources = bo_list;

	if (getenv("RADV_DUMP_CS")) {
		for (unsigned i = 0; i < cs->base.cdw; i++) {
			fprintf(stderr, "0x%08x\n",cs->base.buf[i]);
		}
	}
	r = amdgpu_cs_submit(ctx->ctx, 0, &cs->request, 1);
	if (r) {
		if (r == -ENOMEM)
			fprintf(stderr, "amdgpu: Not enough memory for command submission.\n");
		else
			fprintf(stderr, "amdgpu: The CS has been rejected, "
				"see dmesg for more information.\n");
	}

	amdgpu_bo_list_destroy(bo_list);

	if (fence) {
		fence->context = ctx->ctx;
		fence->ip_type = cs->request.ip_type;
		fence->ip_instance = cs->request.ip_instance;
		fence->ring = cs->request.ring;
		fence->fence = cs->request.seq_no;
	}
	ctx->last_seq_no = cs->request.seq_no;

	return 0;
}

/* add buffer */
/* validate */
/* check space? */
/* is buffer referenced */

static struct radeon_winsys_ctx *amdgpu_ctx_create(struct radeon_winsys *_ws)
{
	struct amdgpu_winsys *ws = amdgpu_winsys(_ws);
	struct amdgpu_ctx *ctx = CALLOC_STRUCT(amdgpu_ctx);
	int r;

	if (!ctx)
		return NULL;
	r = amdgpu_cs_ctx_create(ws->dev, &ctx->ctx);
	if (r) {
		fprintf(stderr, "amdgpu: amdgpu_cs_ctx_create failed. (%i)\n", r);
		goto error_create;
	}
	return (struct radeon_winsys_ctx *)ctx;
error_create:
	return NULL;
}

static void amdgpu_ctx_destroy(struct radeon_winsys_ctx *rwctx)
{
	struct amdgpu_ctx *ctx = (struct amdgpu_ctx *)rwctx;
	amdgpu_cs_ctx_free(ctx->ctx);
	FREE(ctx);
}

static void amdgpu_ctx_wait_idle(struct radeon_winsys_ctx *rwctx)
{
	struct amdgpu_ctx *ctx = (struct amdgpu_ctx *)rwctx;

	if (ctx->last_seq_no) {
		uint32_t expired;
		struct amdgpu_cs_fence fence;

		fence.context = ctx->ctx;
		fence.ip_type = RING_GFX;
		fence.ip_instance = 0;
		fence.ring = 0;
		fence.fence = ctx->last_seq_no;

		amdgpu_cs_query_fence_status(&fence, AMDGPU_TIMEOUT_INFINITE, 0,
					     &expired);
	}
}

void radv_amdgpu_cs_init_functions(struct amdgpu_winsys *ws)
{
	ws->base.ctx_create = amdgpu_ctx_create;
	ws->base.ctx_destroy = amdgpu_ctx_destroy;
	ws->base.ctx_wait_idle = amdgpu_ctx_wait_idle;
	ws->base.cs_create = amdgpu_cs_create;
	ws->base.cs_destroy = amdgpu_cs_destroy;
	ws->base.cs_grow = amdgpu_cs_grow;
	ws->base.cs_finalize = amdgpu_cs_finalize;
	ws->base.cs_reset = amdgpu_cs_reset;
	ws->base.cs_add_buffer = amdgpu_cs_add_buffer;
	ws->base.cs_submit = amdgpu_winsys_cs_submit;
	ws->base.create_fence = amdgpu_create_fence;
	ws->base.destroy_fence = amdgpu_destroy_fence;
	ws->base.fence_wait = amdgpu_fence_wait;
}
