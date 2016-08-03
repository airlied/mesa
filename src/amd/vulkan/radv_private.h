/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 *
 * based in part on anv driver which is:
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

#pragma once

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <pthread.h>
#include <assert.h>
#include <stdint.h>
#include <string.h>
#ifdef HAVE_VALGRIND
#include <valgrind.h>
#include <memcheck.h>
#define VG(x) x
#define __gen_validate_value(x) VALGRIND_CHECK_MEM_IS_DEFINED(&(x), sizeof(x))
#else
#define VG(x)
#endif

#include <amdgpu.h>
#include "radv_device_info.h"
#include "compiler/shader_enums.h"
#include "util/macros.h"
#include "util/list.h"
#include "main/macros.h"
#include "radv_radeon_winsys.h"
#include "ac_binary.h"
#include "ac_nir_to_llvm.h"
#include "radv_descriptor_set.h"

#include <llvm-c/TargetMachine.h>

/* Pre-declarations needed for WSI entrypoints */
struct wl_surface;
struct wl_display;
typedef struct xcb_connection_t xcb_connection_t;
typedef uint32_t xcb_visualid_t;
typedef uint32_t xcb_window_t;

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_intel.h>
#include <vulkan/vk_icd.h>

#include "radv_entrypoints.h"


#define MAX_VBS         32
#define MAX_VERTEX_ATTRIBS 32
#define MAX_RTS          8
#define MAX_VIEWPORTS   16
#define MAX_SCISSORS    16
#define MAX_PUSH_CONSTANTS_SIZE 128
#define MAX_DYNAMIC_BUFFERS 16
#define MAX_IMAGES 8
#define MAX_SAMPLES_LOG2 4 /* SKL supports 16 samples */
#define NUM_META_FS_KEYS 11

#define radv_noreturn __attribute__((__noreturn__))
#define radv_printflike(a, b) __attribute__((__format__(__printf__, a, b)))

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

static inline uint32_t
align_u32(uint32_t v, uint32_t a)
{
	assert(a != 0 && a == (a & -a));
	return (v + a - 1) & ~(a - 1);
}

static inline uint32_t
align_u32_npot(uint32_t v, uint32_t a)
{
	return (v + a - 1) / a * a;
}

static inline uint64_t
align_u64(uint64_t v, uint64_t a)
{
	assert(a != 0 && a == (a & -a));
	return (v + a - 1) & ~(a - 1);
}

static inline int32_t
align_i32(int32_t v, int32_t a)
{
	assert(a != 0 && a == (a & -a));
	return (v + a - 1) & ~(a - 1);
}

/** Alignment must be a power of 2. */
static inline bool
radv_is_aligned(uintmax_t n, uintmax_t a)
{
	assert(a == (a & -a));
	return (n & (a - 1)) == 0;
}

static inline uint32_t
round_up_u32(uint32_t v, uint32_t a)
{
	return (v + a - 1) / a;
}

static inline uint32_t
radv_minify(uint32_t n, uint32_t levels)
{
	if (unlikely(n == 0))
		return 0;
	else
		return MAX(n >> levels, 1);
}
static inline float
radv_clamp_f(float f, float min, float max)
{
	assert(min < max);

	if (f > max)
		return max;
	else if (f < min)
		return min;
	else
		return f;
}

static inline bool
radv_clear_mask(uint32_t *inout_mask, uint32_t clear_mask)
{
	if (*inout_mask & clear_mask) {
		*inout_mask &= ~clear_mask;
		return true;
	} else {
		return false;
	}
}

#define for_each_bit(b, dword)                          \
	for (uint32_t __dword = (dword);		\
	     (b) = __builtin_ffs(__dword) - 1, __dword;	\
	     __dword &= ~(1 << (b)))

#define typed_memcpy(dest, src, count) ({				\
			static_assert(sizeof(*src) == sizeof(*dest), ""); \
			memcpy((dest), (src), (count) * sizeof(*(src))); \
		})

#define zero(x) (memset(&(x), 0, sizeof(x)))

/* Define no kernel as 1, since that's an illegal offset for a kernel */
#define NO_KERNEL 1

struct radv_common {
	VkStructureType                             sType;
	const void*                                 pNext;
};

/* Whenever we generate an error, pass it through this function. Useful for
 * debugging, where we can break on it. Only call at error site, not when
 * propagating errors. Might be useful to plug in a stack trace here.
 */

VkResult __vk_errorf(VkResult error, const char *file, int line, const char *format, ...);

#ifdef DEBUG
#define vk_error(error) __vk_errorf(error, __FILE__, __LINE__, NULL);
#define vk_errorf(error, format, ...) __vk_errorf(error, __FILE__, __LINE__, format, ## __VA_ARGS__);
#else
#define vk_error(error) error
#define vk_errorf(error, format, ...) error
#endif

void __radv_finishme(const char *file, int line, const char *format, ...)
	radv_printflike(3, 4);
void radv_loge(const char *format, ...) radv_printflike(1, 2);
void radv_loge_v(const char *format, va_list va);

/**
 * Print a FINISHME message, including its source location.
 */
#define radv_finishme(format, ...)					\
	__radv_finishme(__FILE__, __LINE__, format, ##__VA_ARGS__);

/* A non-fatal assert.  Useful for debugging. */
#ifdef DEBUG
#define radv_assert(x) ({						\
			if (unlikely(!(x)))				\
				fprintf(stderr, "%s:%d ASSERT: %s\n", __FILE__, __LINE__, #x); \
		})
#else
#define radv_assert(x)
#endif

/**
 * If a block of code is annotated with radv_validate, then the block runs only
 * in debug builds.
 */
#ifdef DEBUG
#define radv_validate if (1)
#else
#define radv_validate if (0)
#endif

void radv_abortf(const char *format, ...) radv_noreturn radv_printflike(1, 2);
void radv_abortfv(const char *format, va_list va) radv_noreturn;

#define stub_return(v)					\
	do {						\
		radv_finishme("stub %s", __func__);	\
		return (v);				\
	} while (0)

#define stub()						\
	do {						\
		radv_finishme("stub %s", __func__);	\
		return;					\
	} while (0)


struct radv_bo {
	struct radeon_winsys_bo *bo;
};

void *radv_resolve_entrypoint(uint32_t index);
void *radv_lookup_entrypoint(const char *name);

extern struct radv_dispatch_table dtable;

#define RADV_CALL(func) ({						\
			if (dtable.func == NULL) {			\
				size_t idx = offsetof(struct radv_dispatch_table, func) / sizeof(void *); \
				dtable.entrypoints[idx] = radv_resolve_entrypoint(idx); \
			}						\
			dtable.func;					\
		})

static inline void *
radv_alloc(const VkAllocationCallbacks *alloc,
	   size_t size, size_t align,
	   VkSystemAllocationScope scope)
{
	return alloc->pfnAllocation(alloc->pUserData, size, align, scope);
}

static inline void *
radv_realloc(const VkAllocationCallbacks *alloc,
	     void *ptr, size_t size, size_t align,
	     VkSystemAllocationScope scope)
{
	return alloc->pfnReallocation(alloc->pUserData, ptr, size, align, scope);
}

static inline void
radv_free(const VkAllocationCallbacks *alloc, void *data)
{
	alloc->pfnFree(alloc->pUserData, data);
}

static inline void *
radv_alloc2(const VkAllocationCallbacks *parent_alloc,
	    const VkAllocationCallbacks *alloc,
	    size_t size, size_t align,
	    VkSystemAllocationScope scope)
{
	if (alloc)
		return radv_alloc(alloc, size, align, scope);
	else
		return radv_alloc(parent_alloc, size, align, scope);
}

static inline void
radv_free2(const VkAllocationCallbacks *parent_alloc,
	   const VkAllocationCallbacks *alloc,
	   void *data)
{
	if (alloc)
		radv_free(alloc, data);
	else
		radv_free(parent_alloc, data);
}
  
struct radv_wsi_interaface;

#define VK_ICD_WSI_PLATFORM_MAX 5

struct radv_physical_device {
	VK_LOADER_DATA                              _loader_data;

	struct radv_instance *                       instance;

	struct radeon_winsys *ws;
	struct radeon_info rad_info;
	uint32_t                                    chipset_id;
	char                                        path[20];
	const char *                                name;
	uint64_t                                    aperture_size;
	int                                         cmd_parser_version;
	uint32_t                    pci_vendor_id;
	uint32_t                    pci_device_id;

	struct radv_wsi_interface *                  wsi[VK_ICD_WSI_PLATFORM_MAX];
};

struct radv_instance {
	VK_LOADER_DATA                              _loader_data;

	VkAllocationCallbacks                       alloc;

	uint32_t                                    apiVersion;
	int                                         physicalDeviceCount;
	struct radv_physical_device                  physicalDevice;
};
 
VkResult radv_init_wsi(struct radv_physical_device *physical_device);
void radv_finish_wsi(struct radv_physical_device *physical_device);

struct radv_meta_state {
	VkAllocationCallbacks alloc;

	/**
	 * Use array element `i` for images with `2^i` samples.
	 */
	struct {
		struct radv_pipeline *color_pipelines[NUM_META_FS_KEYS];

		struct radv_pipeline *depth_only_pipeline;
		struct radv_pipeline *stencil_only_pipeline;
		struct radv_pipeline *depthstencil_pipeline;
	} clear;

	struct {
		VkRenderPass render_pass;

		/** Pipeline that blits from a 1D image. */
		VkPipeline pipeline_1d_src;

		/** Pipeline that blits from a 2D image. */
		VkPipeline pipeline_2d_src;

		/** Pipeline that blits from a 3D image. */
		VkPipeline pipeline_3d_src;

		VkPipelineLayout                          pipeline_layout;
		VkDescriptorSetLayout                     ds_layout;
	} blit;

	struct {
		VkRenderPass render_passes[NUM_META_FS_KEYS];

		VkPipelineLayout p_layouts[2];
		VkDescriptorSetLayout ds_layouts[2];
		VkPipeline pipelines[2][NUM_META_FS_KEYS];
	} blit2d;

	struct {
		VkPipelineLayout                          img_p_layout;
		VkDescriptorSetLayout                     img_ds_layout;
		VkPipeline pipeline;
	} itob;
	struct {
		VkRenderPass render_pass;
		VkPipelineLayout                          img_p_layout;
		VkDescriptorSetLayout                     img_ds_layout;
		VkPipeline pipeline;
	} btoi;

	struct {
		/** Pipeline [i] resolves an image with 2^(i+1) samples.  */
		VkPipeline                                pipelines[MAX_SAMPLES_LOG2];

		VkRenderPass                              pass;
		VkPipelineLayout                          pipeline_layout;
		VkDescriptorSetLayout                     ds_layout;
	} resolve;
};

struct radv_queue {
	VK_LOADER_DATA                              _loader_data;

	struct radv_device *                         device;

	struct radv_state_pool *                     pool;
};

struct radv_pipeline_cache {
	struct radv_device *                          device;
	pthread_mutex_t                              mutex;

	uint32_t                                     total_size;
	uint32_t                                     table_size;
	uint32_t                                     kernel_count;
	uint32_t *                                   hash_table;
};

struct radv_device {
	VK_LOADER_DATA                              _loader_data;

	VkAllocationCallbacks                       alloc;

	struct radv_instance *                       instance;
	uint32_t                                    chipset_id;
	struct radeon_winsys *ws;
	struct radeon_winsys_ctx *hw_ctx;

	struct radv_meta_state                       meta_state;
	struct radv_queue                            queue;
};

void radv_device_get_cache_uuid(void *uuid);

struct radv_device_memory {
	struct radv_bo                               bo;
	uint32_t                                     type_index;
	VkDeviceSize                                 map_size;
	void *                                       map;
};


struct radv_descriptor_range {
	uint64_t va;
	uint32_t size;
};

struct radv_descriptor_set {
	const struct radv_descriptor_set_layout *layout;
	struct list_head descriptor_pool;
	uint32_t size;
	uint32_t buffer_count;
	struct radv_buffer_view *buffer_views;
	struct radv_bo bo;
	uint64_t va;
	uint32_t *mapped_ptr;
	struct radv_descriptor_range *dynamic_descriptors;
	struct radv_bo *descriptors[0];
};

struct radv_descriptor_pool {
	struct list_head descriptor_sets;
};

struct radv_buffer {
	struct radv_device *                          device;
	VkDeviceSize                                 size;

	VkBufferUsageFlags                           usage;

	/* Set when bound */
	struct radv_bo *                              bo;
	VkDeviceSize                                 offset;
};


enum radv_cmd_dirty_bits {
	RADV_CMD_DIRTY_DYNAMIC_VIEWPORT                  = 1 << 0, /* VK_DYNAMIC_STATE_VIEWPORT */
	RADV_CMD_DIRTY_DYNAMIC_SCISSOR                   = 1 << 1, /* VK_DYNAMIC_STATE_SCISSOR */
	RADV_CMD_DIRTY_DYNAMIC_LINE_WIDTH                = 1 << 2, /* VK_DYNAMIC_STATE_LINE_WIDTH */
	RADV_CMD_DIRTY_DYNAMIC_DEPTH_BIAS                = 1 << 3, /* VK_DYNAMIC_STATE_DEPTH_BIAS */
	RADV_CMD_DIRTY_DYNAMIC_BLEND_CONSTANTS           = 1 << 4, /* VK_DYNAMIC_STATE_BLEND_CONSTANTS */
	RADV_CMD_DIRTY_DYNAMIC_DEPTH_BOUNDS              = 1 << 5, /* VK_DYNAMIC_STATE_DEPTH_BOUNDS */
	RADV_CMD_DIRTY_DYNAMIC_STENCIL_COMPARE_MASK      = 1 << 6, /* VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK */
	RADV_CMD_DIRTY_DYNAMIC_STENCIL_WRITE_MASK        = 1 << 7, /* VK_DYNAMIC_STATE_STENCIL_WRITE_MASK */
	RADV_CMD_DIRTY_DYNAMIC_STENCIL_REFERENCE         = 1 << 8, /* VK_DYNAMIC_STATE_STENCIL_REFERENCE */
	RADV_CMD_DIRTY_DYNAMIC_ALL                       = (1 << 9) - 1,
	RADV_CMD_DIRTY_PIPELINE                          = 1 << 9,
	RADV_CMD_DIRTY_INDEX_BUFFER                      = 1 << 10,
	RADV_CMD_DIRTY_RENDER_TARGETS                    = 1 << 11,
};
typedef uint32_t radv_cmd_dirty_mask_t;

enum radv_cmd_flush_bits {
	RADV_CMD_FLAG_INV_ICACHE = 1 << 0,
	/* SMEM L1, other names: KCACHE, constant cache, DCACHE, data cache */
	RADV_CMD_FLAG_INV_SMEM_L1 = 1 << 1,
	/* VMEM L1 can optionally be bypassed (GLC=1). Other names: TC L1 */
	RADV_CMD_FLAG_INV_VMEM_L1 = 1 << 2,
	/* Used by everything except CB/DB, can be bypassed (SLC=1). Other names: TC L2 */
	RADV_CMD_FLAG_INV_GLOBAL_L2 = 1 << 3,
	/* Framebuffer caches */
	RADV_CMD_FLAG_FLUSH_AND_INV_CB_META = 1 << 4,
	RADV_CMD_FLAG_FLUSH_AND_INV_DB_META = 1 << 5,
	RADV_CMD_FLAG_FLUSH_AND_INV_DB = 1 << 6,
	RADV_CMD_FLAG_FLUSH_AND_INV_CB = 1 << 7,
	/* Engine synchronization. */
	RADV_CMD_FLAG_VS_PARTIAL_FLUSH = 1 << 8,
	RADV_CMD_FLAG_PS_PARTIAL_FLUSH = 1 << 9,
	RADV_CMD_FLAG_CS_PARTIAL_FLUSH = 1 << 10,
	RADV_CMD_FLAG_VGT_FLUSH        = 1 << 11,

	RADV_CMD_FLUSH_AND_INV_FRAMEBUFFER = (RADV_CMD_FLAG_FLUSH_AND_INV_CB |
					      RADV_CMD_FLAG_FLUSH_AND_INV_CB_META |
					      RADV_CMD_FLAG_FLUSH_AND_INV_DB |
					      RADV_CMD_FLAG_FLUSH_AND_INV_DB_META)
};

struct radv_vertex_binding {
	struct radv_buffer *                          buffer;
	VkDeviceSize                                 offset;
};

struct radv_dynamic_state {
	struct {
		uint32_t                                  count;
		VkViewport                                viewports[MAX_VIEWPORTS];
	} viewport;

	struct {
		uint32_t                                  count;
		VkRect2D                                  scissors[MAX_SCISSORS];
	} scissor;

	float                                        line_width;

	struct {
		float                                     bias;
		float                                     clamp;
		float                                     slope;
	} depth_bias;

	float                                        blend_constants[4];

	struct {
		float                                     min;
		float                                     max;
	} depth_bounds;

	struct {
		uint32_t                                  front;
		uint32_t                                  back;
	} stencil_compare_mask;

	struct {
		uint32_t                                  front;
		uint32_t                                  back;
	} stencil_write_mask;

	struct {
		uint32_t                                  front;
		uint32_t                                  back;
	} stencil_reference;
};

extern const struct radv_dynamic_state default_dynamic_state;

void radv_dynamic_state_copy(struct radv_dynamic_state *dest,
			     const struct radv_dynamic_state *src,
			     uint32_t copy_mask);
/**
 * Attachment state when recording a renderpass instance.
 *
 * The clear value is valid only if there exists a pending clear.
 */
struct radv_attachment_state {
	VkImageAspectFlags                           pending_clear_aspects;
	VkClearValue                                 clear_value;
};

struct radv_cmd_state {
	uint32_t                                      vb_dirty;
	bool                                          vertex_descriptors_dirty;
	radv_cmd_dirty_mask_t                         dirty;
	radv_cmd_dirty_mask_t                         compute_dirty;

	struct radv_pipeline *                        pipeline;
	struct radv_pipeline *                        compute_pipeline;
	struct radv_framebuffer *                     framebuffer;
	struct radv_render_pass *                     pass;
	struct radv_subpass *                         subpass;
	struct radv_dynamic_state                     dynamic;
	struct radv_vertex_binding                    vertex_bindings[MAX_VBS];
	struct radv_descriptor_set *                  descriptors[MAX_SETS];
	VkShaderStageFlags                           descriptors_dirty;
	struct radv_attachment_state *                attachments;
	VkRect2D                                     render_area;
	struct radv_buffer *                         index_buffer;
	uint32_t                                     index_type;
	uint32_t                                     index_offset;
	enum radv_cmd_flush_bits                     flush_bits;
};
struct radv_cmd_pool {
	VkAllocationCallbacks                        alloc;
	struct list_head                             cmd_buffers;
};

struct radv_cmd_buffer_upload {
	uint8_t *map;
	unsigned offset;
	uint64_t size;
	struct radv_bo upload_bo;
	struct list_head list;
};

struct radv_cmd_buffer {
	VK_LOADER_DATA                               _loader_data;

	struct radv_device *                          device;

	struct radv_cmd_pool *                        pool;
	struct list_head                             pool_link;

	VkCommandBufferUsageFlags                    usage_flags;
	VkCommandBufferLevel                         level;
	struct radeon_winsys_cs *cs;
	struct radv_cmd_state state;

	uint8_t push_constants[MAX_PUSH_CONSTANTS_SIZE];
	uint32_t dynamic_buffers[16 * MAX_DYNAMIC_BUFFERS];
	VkShaderStageFlags push_constant_stages;

	struct radv_bo border_color_bo;
	struct radv_cmd_buffer_upload upload;
	uint32_t texture_border_offset;

	bool record_fail;
};

void si_init_config(struct radv_physical_device *physical_device,
		    struct radv_cmd_buffer *cmd_buffer);
void si_write_viewport(struct radeon_winsys_cs *cs, int first_vp,
		       int count, const VkViewport *viewports);
void si_write_scissors(struct radeon_winsys_cs *cs, int first,
		       int count, const VkRect2D *scissors);
uint32_t si_get_ia_multi_vgt_param(struct radv_cmd_buffer *cmd_buffer);
void si_emit_cache_flush(struct radv_cmd_buffer *cmd_buffer);
void si_cp_dma_buffer_copy(struct radv_cmd_buffer *cmd_buffer,
			   uint64_t src_va, uint64_t dest_va,
			   uint64_t size);
void si_cp_dma_clear_buffer(struct radv_cmd_buffer *cmd_buffer, uint64_t va,
			    uint64_t size, unsigned value);
bool
radv_cmd_buffer_upload_alloc(struct radv_cmd_buffer *cmd_buffer,
			     unsigned size,
			     unsigned alignment,
			     unsigned *out_offset,
			     void **ptr);

bool
radv_cmd_buffer_upload_data(struct radv_cmd_buffer *cmd_buffer,
			    unsigned size, unsigned alignmnet,
			    const void *data, unsigned *out_offset);
void radv_cmd_buffer_clear_subpass(struct radv_cmd_buffer *cmd_buffer);
void radv_cmd_buffer_resolve_subpass(struct radv_cmd_buffer *cmd_buffer);

/*
 * Takes x,y,z as exact numbers of invocations, instead of blocks.
 *
 * Limitations: Can't call normal dispatch functions without binding or rebinding
 *              the compute pipeline.
 */
void radv_unaligned_dispatch(
	struct radv_cmd_buffer                      *cmd_buffer,
	uint32_t                                    x,
	uint32_t                                    y,
	uint32_t                                    z);

struct radv_event {
	struct radeon_winsys_bo *bo;
	uint64_t *map;
};

struct nir_shader;

struct radv_shader_module {
	struct nir_shader *                          nir;
	unsigned char                                sha1[20];
	uint32_t                                     size;
	char                                         data[0];
};

void radv_hash_shader(unsigned char *hash, const void *key, size_t key_size,
		      struct radv_shader_module *module,
		      const char *entrypoint,
		      const VkSpecializationInfo *spec_info);

static inline gl_shader_stage
vk_to_mesa_shader_stage(VkShaderStageFlagBits vk_stage)
{
	assert(__builtin_popcount(vk_stage) == 1);
	return ffs(vk_stage) - 1;
}

static inline VkShaderStageFlagBits
mesa_to_vk_shader_stage(gl_shader_stage mesa_stage)
{
	return (1 << mesa_stage);
}

#define RADV_STAGE_MASK ((1 << MESA_SHADER_STAGES) - 1)

#define radv_foreach_stage(stage, stage_bits)				\
	for (gl_shader_stage stage,					\
		     __tmp = (gl_shader_stage)((stage_bits) & RADV_STAGE_MASK);	\
	     stage = __builtin_ffs(__tmp) - 1, __tmp;			\
	     __tmp &= ~(1 << (stage)))

struct radv_shader_variant {
	struct radeon_winsys_bo *bo;
	struct ac_shader_config config;
	struct ac_shader_variant_info info;
	unsigned rsrc1;
	unsigned rsrc2;
};

struct radv_depth_stencil_state {
	uint32_t db_depth_control;
	uint32_t db_stencil_control;
	uint32_t db_depth_bounds_min;
	uint32_t db_depth_bounds_max;
};

struct radv_blend_state {
	uint32_t cb_color_control;
	uint32_t cb_target_mask;
	uint32_t sx_mrt0_blend_opt[8];
	uint32_t cb_blend_control[8];

	uint32_t spi_shader_col_format;
	uint32_t cb_shader_mask;
};

unsigned radv_format_meta_fs_key(VkFormat format);

struct radv_raster_state {
	uint32_t pa_cl_clip_cntl;
	uint32_t pa_cl_vs_out_cntl;
	uint32_t spi_interp_control;
	uint32_t pa_su_point_size;
	uint32_t pa_su_point_minmax;
	uint32_t pa_su_line_cntl;
	uint32_t pa_sc_line_cntl;
	uint32_t pa_sc_mode_cntl_0;
	uint32_t pa_su_vtx_cntl;
	uint32_t pa_su_poly_offset_clamp;
	uint32_t pa_su_sc_mode_cntl;
	uint32_t pa_su_poly_offset_front_scale;
	uint32_t pa_su_poly_offset_front_offset;
	uint32_t pa_su_poly_offset_back_scale;
	uint32_t pa_su_poly_offset_back_offset;
};

struct radv_pipeline {
	struct radv_device *                          device;
	uint32_t                                     dynamic_state_mask;
	struct radv_dynamic_state                     dynamic_state;

	struct radv_pipeline_layout *                 layout;

	bool                                         needs_data_cache;

	struct radv_shader_variant *                 shaders[MESA_SHADER_STAGES];
	VkShaderStageFlags                           active_stages;

	uint32_t va_rsrc_word3[MAX_VERTEX_ATTRIBS];
	uint32_t va_binding[MAX_VERTEX_ATTRIBS];
	uint32_t va_offset[MAX_VERTEX_ATTRIBS];
	uint32_t num_vertex_attribs;
	uint32_t                                     binding_stride[MAX_VBS];
	bool                                         instancing_enable[MAX_VBS];

	union {
		struct {
			struct radv_blend_state blend;
			struct radv_depth_stencil_state ds;
			struct radv_raster_state raster;

			unsigned prim;
			bool prim_restart_enable;
		} graphics;
		struct {
			int block_size[3];
		} compute;
	};
};

struct radv_graphics_pipeline_create_info {
	bool                                         use_rectlist;
};

VkResult
radv_pipeline_init(struct radv_pipeline *pipeline, struct radv_device *device,
		   struct radv_pipeline_cache *cache,
		   const VkGraphicsPipelineCreateInfo *pCreateInfo,
		   const struct radv_graphics_pipeline_create_info *extra,
		   const VkAllocationCallbacks *alloc);

VkResult
radv_graphics_pipeline_create(VkDevice device,
			      VkPipelineCache cache,
			      const VkGraphicsPipelineCreateInfo *pCreateInfo,
			      const struct radv_graphics_pipeline_create_info *extra,
			      const VkAllocationCallbacks *alloc,
			      VkPipeline *pPipeline);

struct vk_format_description;
uint32_t radv_translate_buffer_dataformat(const struct vk_format_description *desc,
					  int first_non_void);
uint32_t radv_translate_buffer_numformat(const struct vk_format_description *desc,
					 int first_non_void);
uint32_t radv_translate_colorformat(VkFormat format);
uint32_t radv_translate_color_numformat(VkFormat format,
					const struct vk_format_description *desc,
					int first_non_void);
uint32_t radv_colorformat_endian_swap(uint32_t colorformat);
unsigned radv_translate_colorswap(VkFormat format, bool do_endian_swap);
uint32_t radv_translate_dbformat(VkFormat format);
uint32_t radv_translate_tex_dataformat(VkFormat format,
				       const struct vk_format_description *desc,
				       int first_non_void);
uint32_t radv_translate_tex_numformat(VkFormat format,
				      const struct vk_format_description *desc,
				      int first_non_void);

struct radv_image {
	VkImageType type;
	/* The original VkFormat provided by the client.  This may not match any
	 * of the actual surface formats.
	 */
	VkFormat vk_format;
	VkImageAspectFlags aspects;
	VkExtent3D extent;
	uint32_t levels;
	uint32_t array_size;
	uint32_t samples; /**< VkImageCreateInfo::samples */
	VkImageUsageFlags usage; /**< Superset of VkImageCreateInfo::usage. */
	VkImageTiling tiling; /** VkImageCreateInfo::tiling */

	VkDeviceSize size;
	uint32_t alignment;

	/* Set when bound */
	struct radv_bo *bo;
	VkDeviceSize offset;
	uint32_t dcc_offset;
	struct radeon_surf surface;
};

static inline uint32_t
radv_get_layerCount(const struct radv_image *image,
		    const VkImageSubresourceRange *range)
{
	return range->layerCount == VK_REMAINING_ARRAY_LAYERS ?
		image->array_size - range->baseArrayLayer : range->layerCount;
}

static inline uint32_t
radv_get_levelCount(const struct radv_image *image,
		    const VkImageSubresourceRange *range)
{
	return range->levelCount == VK_REMAINING_MIP_LEVELS ?
		image->levels - range->baseMipLevel : range->levelCount;
}

struct radeon_bo_metadata;
void
radv_init_metadata(struct radv_device *device,
		   struct radv_image *image,
		   struct radeon_bo_metadata *metadata);

struct radv_image_view {
	const struct radv_image *image; /**< VkImageViewCreateInfo::image */
	struct radv_bo *bo;
	uint32_t offset; /**< Offset into bo. */

	VkImageViewType type;
	VkImageAspectFlags aspect_mask;
	VkFormat vk_format;
	uint32_t base_layer;
	uint32_t base_mip;
	VkExtent3D extent; /**< Extent of VkImageViewCreateInfo::baseMipLevel. */

	uint32_t descriptor[8];
	uint32_t fmask_descriptor[8];
};

struct radv_image_create_info {
	const VkImageCreateInfo *vk_info;
	uint32_t stride;
	bool scanout;
};

VkResult radv_image_create(VkDevice _device,
			   const struct radv_image_create_info *info,
			   const VkAllocationCallbacks* alloc,
			   VkImage *pImage);

void radv_image_view_init(struct radv_image_view *view,
			  struct radv_device *device,
			  const VkImageViewCreateInfo* pCreateInfo,
			  struct radv_cmd_buffer *cmd_buffer,
			  VkImageUsageFlags usage_mask);

struct radv_buffer_view {
	struct radv_bo *bo;
	VkFormat vk_format;
	uint32_t offset; /**< Offset into bo. */
	uint64_t range; /**< VkBufferViewCreateInfo::range */
	uint32_t state[4];
};
void radv_buffer_view_init(struct radv_buffer_view *view,
			   struct radv_device *device,
			   const VkBufferViewCreateInfo* pCreateInfo,
			   struct radv_cmd_buffer *cmd_buffer);

static inline struct VkExtent3D
radv_sanitize_image_extent(const VkImageType imageType,
			   const struct VkExtent3D imageExtent)
{
	switch (imageType) {
	case VK_IMAGE_TYPE_1D:
		return (VkExtent3D) { imageExtent.width, 1, 1 };
	case VK_IMAGE_TYPE_2D:
		return (VkExtent3D) { imageExtent.width, imageExtent.height, 1 };
	case VK_IMAGE_TYPE_3D:
		return imageExtent;
	default:
		unreachable("invalid image type");
	}
}

static inline struct VkOffset3D
radv_sanitize_image_offset(const VkImageType imageType,
			   const struct VkOffset3D imageOffset)
{
	switch (imageType) {
	case VK_IMAGE_TYPE_1D:
		return (VkOffset3D) { imageOffset.x, 0, 0 };
	case VK_IMAGE_TYPE_2D:
		return (VkOffset3D) { imageOffset.x, imageOffset.y, 0 };
	case VK_IMAGE_TYPE_3D:
		return imageOffset;
	default:
		unreachable("invalid image type");
	}
}

struct radv_sampler {
	uint32_t state[4];
};

struct radv_color_buffer_info {
	uint32_t color_index;
	uint32_t cb_color_base;
	uint32_t cb_color_pitch;
	uint32_t cb_color_slice;
	uint32_t cb_color_view;
	uint32_t cb_color_info;
	uint32_t cb_color_attrib;
	uint32_t cb_dcc_control;
	uint32_t cb_color_cmask;
	uint32_t cb_color_cmask_slice;
	uint32_t cb_color_fmask;
	uint32_t cb_color_fmask_slice;
	uint32_t cb_clear_value0;
	uint32_t cb_clear_value1;
};

struct radv_ds_buffer_info {
	uint32_t db_depth_info;
	uint32_t db_z_info;
	uint32_t db_stencil_info;
	uint32_t db_z_read_base;
	uint32_t db_stencil_read_base;
	uint32_t db_z_write_base;
	uint32_t db_stencil_write_base;
	uint32_t db_depth_view;
	uint32_t db_depth_size;
	uint32_t db_depth_slice;
	uint32_t db_stencil_clear;
	uint32_t db_depth_clear;
	uint32_t db_htile_surface;
	uint32_t db_htile_data_base;
	uint32_t pa_su_poly_offset_db_fmt_cntl;
};

struct radv_attachment_info {
	union {
		struct radv_color_buffer_info cb;
		struct radv_ds_buffer_info ds;
	};
	struct radv_image_view *attachment;
};

struct radv_framebuffer {
	uint32_t                                     width;
	uint32_t                                     height;
	uint32_t                                     layers;

	uint32_t                                     attachment_count;
	struct radv_attachment_info                  attachments[0];
};

   
struct radv_subpass {
	uint32_t                                     input_count;
	uint32_t *                                   input_attachments;
	uint32_t                                     color_count;
	uint32_t *                                   color_attachments;
	uint32_t *                                   resolve_attachments;
	uint32_t                                     depth_stencil_attachment;

	/** Subpass has at least one resolve attachment */
	bool                                         has_resolve;
};

struct radv_render_pass_attachment {
	VkFormat                                     format;
	uint32_t                                     samples;
	VkAttachmentLoadOp                           load_op;
	VkAttachmentLoadOp                           stencil_load_op;
};

struct radv_render_pass {
	uint32_t                                     attachment_count;
	uint32_t                                     subpass_count;
	uint32_t *                                   subpass_attachments;
	struct radv_render_pass_attachment *          attachments;
	struct radv_subpass                           subpasses[0];
};

VkResult radv_device_init_meta(struct radv_device *device);
void radv_device_finish_meta(struct radv_device *device);

VkResult
radv_temp_descriptor_set_create(struct radv_device *device,
				struct radv_cmd_buffer *cmd_buffer,
				VkDescriptorSetLayout _layout,
				VkDescriptorSet *_set);

void
radv_temp_descriptor_set_destroy(struct radv_device *device,
				 VkDescriptorSet _set);

#define RADV_DEFINE_HANDLE_CASTS(__radv_type, __VkType)		\
								\
	static inline struct __radv_type *			\
	__radv_type ## _from_handle(__VkType _handle)		\
	{							\
		return (struct __radv_type *) _handle;		\
	}							\
								\
	static inline __VkType					\
	__radv_type ## _to_handle(struct __radv_type *_obj)	\
	{							\
		return (__VkType) _obj;				\
	}

#define RADV_DEFINE_NONDISP_HANDLE_CASTS(__radv_type, __VkType)		\
									\
	static inline struct __radv_type *				\
	__radv_type ## _from_handle(__VkType _handle)			\
	{								\
		return (struct __radv_type *)(uintptr_t) _handle;	\
	}								\
									\
	static inline __VkType						\
	__radv_type ## _to_handle(struct __radv_type *_obj)		\
	{								\
		return (__VkType)(uintptr_t) _obj;			\
	}

#define RADV_FROM_HANDLE(__radv_type, __name, __handle)			\
	struct __radv_type *__name = __radv_type ## _from_handle(__handle)

RADV_DEFINE_HANDLE_CASTS(radv_cmd_buffer, VkCommandBuffer)
RADV_DEFINE_HANDLE_CASTS(radv_device, VkDevice)
RADV_DEFINE_HANDLE_CASTS(radv_instance, VkInstance)
RADV_DEFINE_HANDLE_CASTS(radv_physical_device, VkPhysicalDevice)
RADV_DEFINE_HANDLE_CASTS(radv_queue, VkQueue)

RADV_DEFINE_NONDISP_HANDLE_CASTS(radv_cmd_pool, VkCommandPool)
RADV_DEFINE_NONDISP_HANDLE_CASTS(radv_buffer, VkBuffer)
RADV_DEFINE_NONDISP_HANDLE_CASTS(radv_buffer_view, VkBufferView)
RADV_DEFINE_NONDISP_HANDLE_CASTS(radv_descriptor_pool, VkDescriptorPool)
RADV_DEFINE_NONDISP_HANDLE_CASTS(radv_descriptor_set, VkDescriptorSet)
RADV_DEFINE_NONDISP_HANDLE_CASTS(radv_descriptor_set_layout, VkDescriptorSetLayout)
RADV_DEFINE_NONDISP_HANDLE_CASTS(radv_device_memory, VkDeviceMemory)
RADV_DEFINE_NONDISP_HANDLE_CASTS(radv_fence, VkFence)
RADV_DEFINE_NONDISP_HANDLE_CASTS(radv_event, VkEvent)
RADV_DEFINE_NONDISP_HANDLE_CASTS(radv_framebuffer, VkFramebuffer)
RADV_DEFINE_NONDISP_HANDLE_CASTS(radv_image, VkImage)
RADV_DEFINE_NONDISP_HANDLE_CASTS(radv_image_view, VkImageView);
RADV_DEFINE_NONDISP_HANDLE_CASTS(radv_pipeline_cache, VkPipelineCache)
RADV_DEFINE_NONDISP_HANDLE_CASTS(radv_pipeline, VkPipeline)
RADV_DEFINE_NONDISP_HANDLE_CASTS(radv_pipeline_layout, VkPipelineLayout)
RADV_DEFINE_NONDISP_HANDLE_CASTS(radv_query_pool, VkQueryPool)
RADV_DEFINE_NONDISP_HANDLE_CASTS(radv_render_pass, VkRenderPass)
RADV_DEFINE_NONDISP_HANDLE_CASTS(radv_sampler, VkSampler)
RADV_DEFINE_NONDISP_HANDLE_CASTS(radv_shader_module, VkShaderModule)

#define RADV_DEFINE_STRUCT_CASTS(__radv_type, __VkType)			\
									\
	static inline const __VkType *					\
	__radv_type ## _to_ ## __VkType(const struct __radv_type *__radv_obj) \
	{								\
		return (const __VkType *) __radv_obj;			\
	}

#define RADV_COMMON_TO_STRUCT(__VkType, __vk_name, __common_name)	\
	const __VkType *__vk_name = radv_common_to_ ## __VkType(__common_name)

RADV_DEFINE_STRUCT_CASTS(radv_common, VkMemoryBarrier)
RADV_DEFINE_STRUCT_CASTS(radv_common, VkBufferMemoryBarrier)
RADV_DEFINE_STRUCT_CASTS(radv_common, VkImageMemoryBarrier)


