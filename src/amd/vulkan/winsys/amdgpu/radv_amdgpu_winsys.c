/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 * based on amdgpu winsys.
 * Copyright © 2011 Marek Olšák <maraeo@gmail.com>
 * Copyright © 2015 Advanced Micro Devices, Inc.
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
#include "radv_amdgpu_winsys.h"
#include "radv_amdgpu_winsys_public.h"
#include "radv_amdgpu_surface.h"
#include "amdgpu_id.h"
#include "xf86drm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <amdgpu_drm.h>
#include <assert.h>
#include "radv_amdgpu_cs.h"
#include "radv_amdgpu_bo.h"
#include "radv_amdgpu_surface.h"
#define CIK_TILE_MODE_COLOR_2D			14

#define CIK__GB_TILE_MODE__PIPE_CONFIG(x)        (((x) >> 6) & 0x1f)
#define     CIK__PIPE_CONFIG__ADDR_SURF_P2               0
#define     CIK__PIPE_CONFIG__ADDR_SURF_P4_8x16          4
#define     CIK__PIPE_CONFIG__ADDR_SURF_P4_16x16         5
#define     CIK__PIPE_CONFIG__ADDR_SURF_P4_16x32         6
#define     CIK__PIPE_CONFIG__ADDR_SURF_P4_32x32         7
#define     CIK__PIPE_CONFIG__ADDR_SURF_P8_16x16_8x16    8
#define     CIK__PIPE_CONFIG__ADDR_SURF_P8_16x32_8x16    9
#define     CIK__PIPE_CONFIG__ADDR_SURF_P8_32x32_8x16    10
#define     CIK__PIPE_CONFIG__ADDR_SURF_P8_16x32_16x16   11
#define     CIK__PIPE_CONFIG__ADDR_SURF_P8_32x32_16x16   12
#define     CIK__PIPE_CONFIG__ADDR_SURF_P8_32x32_16x32   13
#define     CIK__PIPE_CONFIG__ADDR_SURF_P8_32x64_32x32   14
#define     CIK__PIPE_CONFIG__ADDR_SURF_P16_32X32_8X16   16
#define     CIK__PIPE_CONFIG__ADDR_SURF_P16_32X32_16X16  17

static unsigned cik_get_num_tile_pipes(struct amdgpu_gpu_info *info)
{
	unsigned mode2d = info->gb_tile_mode[CIK_TILE_MODE_COLOR_2D];

	switch (CIK__GB_TILE_MODE__PIPE_CONFIG(mode2d)) {
	case CIK__PIPE_CONFIG__ADDR_SURF_P2:
		return 2;
	case CIK__PIPE_CONFIG__ADDR_SURF_P4_8x16:
	case CIK__PIPE_CONFIG__ADDR_SURF_P4_16x16:
	case CIK__PIPE_CONFIG__ADDR_SURF_P4_16x32:
	case CIK__PIPE_CONFIG__ADDR_SURF_P4_32x32:
		return 4;
	case CIK__PIPE_CONFIG__ADDR_SURF_P8_16x16_8x16:
	case CIK__PIPE_CONFIG__ADDR_SURF_P8_16x32_8x16:
	case CIK__PIPE_CONFIG__ADDR_SURF_P8_32x32_8x16:
	case CIK__PIPE_CONFIG__ADDR_SURF_P8_16x32_16x16:
	case CIK__PIPE_CONFIG__ADDR_SURF_P8_32x32_16x16:
	case CIK__PIPE_CONFIG__ADDR_SURF_P8_32x32_16x32:
	case CIK__PIPE_CONFIG__ADDR_SURF_P8_32x64_32x32:
		return 8;
	case CIK__PIPE_CONFIG__ADDR_SURF_P16_32X32_8X16:
	case CIK__PIPE_CONFIG__ADDR_SURF_P16_32X32_16X16:
		return 16;
	default:
		fprintf(stderr, "Invalid CIK pipe configuration, assuming P2\n");
		assert(!"this should never occur");
		return 2;
	}
}

static bool
do_winsys_init(struct amdgpu_winsys *ws, int fd)
{
	struct amdgpu_buffer_size_alignments alignment_info = {};
	struct amdgpu_heap_info vram, gtt;
	struct drm_amdgpu_info_hw_ip dma = {};
	drmDevicePtr devinfo;
	int r;
	/* Get PCI info. */
	r = drmGetDevice(fd, &devinfo);
	if (r) {
		fprintf(stderr, "amdgpu: drmGetDevice failed.\n");
		goto fail;
	}
	ws->info.pci_domain = devinfo->businfo.pci->domain;
	ws->info.pci_bus = devinfo->businfo.pci->bus;
	ws->info.pci_dev = devinfo->businfo.pci->dev;
	ws->info.pci_func = devinfo->businfo.pci->func;
	drmFreeDevice(&devinfo);

	/* Query hardware and driver information. */
	r = amdgpu_query_gpu_info(ws->dev, &ws->amdinfo);
	if (r) {
		fprintf(stderr, "amdgpu: amdgpu_query_gpu_info failed.\n");
		goto fail;
	}

	r = amdgpu_query_buffer_size_alignment(ws->dev, &alignment_info);
	if (r) {
		fprintf(stderr, "amdgpu: amdgpu_query_buffer_size_alignment failed.\n");
		goto fail;
	}

	r = amdgpu_query_heap_info(ws->dev, AMDGPU_GEM_DOMAIN_VRAM, 0, &vram);
	if (r) {
		fprintf(stderr, "amdgpu: amdgpu_query_heap_info(vram) failed.\n");
		goto fail;
	}

	r = amdgpu_query_heap_info(ws->dev, AMDGPU_GEM_DOMAIN_GTT, 0, &gtt);
	if (r) {
		fprintf(stderr, "amdgpu: amdgpu_query_heap_info(gtt) failed.\n");
		goto fail;
	}

	r = amdgpu_query_hw_ip_info(ws->dev, AMDGPU_HW_IP_DMA, 0, &dma);
	if (r) {
		fprintf(stderr, "amdgpu: amdgpu_query_hw_ip_info(dma) failed.\n");
		goto fail;
	}
	ws->info.pci_id = ws->amdinfo.asic_id; /* TODO: is this correct? */
	ws->info.vce_harvest_config = ws->amdinfo.vce_harvest_config;

	switch (ws->info.pci_id) {
#define CHIPSET(pci_id, name, cfamily) case pci_id: ws->info.family = CHIP_##cfamily; break;
#include "pci_ids/radeonsi_pci_ids.h"
#undef CHIPSET
	default:
		fprintf(stderr, "amdgpu: Invalid PCI ID.\n");
		goto fail;
	}

	if (ws->info.family >= CHIP_TONGA)
		ws->info.chip_class = VI;
	else if (ws->info.family >= CHIP_BONAIRE)
		ws->info.chip_class = CIK;
	else {
		fprintf(stderr, "amdgpu: Unknown family.\n");
		goto fail;
	}

	/* family and rev_id are for addrlib */
	switch (ws->info.family) {
	case CHIP_BONAIRE:
		ws->family = FAMILY_CI;
		ws->rev_id = CI_BONAIRE_M_A0;
		break;
	case CHIP_KAVERI:
		ws->family = FAMILY_KV;
		ws->rev_id = KV_SPECTRE_A0;
		break;
	case CHIP_KABINI:
		ws->family = FAMILY_KV;
		ws->rev_id = KB_KALINDI_A0;
		break;
	case CHIP_HAWAII:
		ws->family = FAMILY_CI;
		ws->rev_id = CI_HAWAII_P_A0;
		break;
	case CHIP_MULLINS:
		ws->family = FAMILY_KV;
		ws->rev_id = ML_GODAVARI_A0;
		break;
	case CHIP_TONGA:
		ws->family = FAMILY_VI;
		ws->rev_id = VI_TONGA_P_A0;
		break;
	case CHIP_ICELAND:
		ws->family = FAMILY_VI;
		ws->rev_id = VI_ICELAND_M_A0;
		break;
	case CHIP_CARRIZO:
		ws->family = FAMILY_CZ;
		ws->rev_id = CARRIZO_A0;
		break;
	case CHIP_STONEY:
		ws->family = FAMILY_CZ;
		ws->rev_id = STONEY_A0;
		break;
	case CHIP_FIJI:
		ws->family = FAMILY_VI;
		ws->rev_id = VI_FIJI_P_A0;
		break;
	case CHIP_POLARIS10:
		ws->family = FAMILY_VI;
		ws->rev_id = VI_POLARIS10_P_A0;
		break;
	case CHIP_POLARIS11:
		ws->family = FAMILY_VI;
		ws->rev_id = VI_POLARIS11_M_A0;
		break;
	default:
		fprintf(stderr, "amdgpu: Unknown family.\n");
		goto fail;
	}
   
	ws->addrlib = radv_amdgpu_addr_create(&ws->amdinfo, ws->family, ws->rev_id);
	if (!ws->addrlib) {
		fprintf(stderr, "amdgpu: Cannot create addrlib.\n");
		goto fail;
	}
	/* Set hardware information. */
	ws->info.gart_size = gtt.heap_size;
	ws->info.vram_size = vram.heap_size;
	/* convert the shader clock from KHz to MHz */
	ws->info.max_shader_clock = ws->amdinfo.max_engine_clk / 1000;
	ws->info.max_se = ws->amdinfo.num_shader_engines;
	ws->info.max_sh_per_se = ws->amdinfo.num_shader_arrays_per_engine;
	ws->info.has_uvd = 0;
	ws->info.vce_fw_version = 0;
	ws->info.has_userptr = TRUE;
	ws->info.num_render_backends = ws->amdinfo.rb_pipes;
	ws->info.clock_crystal_freq = ws->amdinfo.gpu_counter_freq;
	ws->info.num_tile_pipes = cik_get_num_tile_pipes(&ws->amdinfo);
	ws->info.pipe_interleave_bytes = 256 << ((ws->amdinfo.gb_addr_cfg >> 4) & 0x7);
	ws->info.has_virtual_memory = TRUE;
	ws->info.has_sdma = dma.available_rings != 0;

#if 0
	/* Get the number of good compute units. */
	ws->info.num_good_compute_units = 0;
	for (i = 0; i < ws->info.max_se; i++)
		for (j = 0; j < ws->info.max_sh_per_se; j++)
			ws->info.num_good_compute_units +=
				util_bitcount(ws->amdinfo.cu_bitmap[i][j]);
#endif
	memcpy(ws->info.si_tile_mode_array, ws->amdinfo.gb_tile_mode,
	       sizeof(ws->amdinfo.gb_tile_mode));
	ws->info.enabled_rb_mask = ws->amdinfo.enabled_rb_pipes_mask;

	memcpy(ws->info.cik_macrotile_mode_array, ws->amdinfo.gb_macro_tile_mode,
	       sizeof(ws->amdinfo.gb_macro_tile_mode));

	ws->info.gart_page_size = alignment_info.size_remote;
	return true;
fail:
	return false;
}

static void amdgpu_winsys_query_info(struct radeon_winsys *rws,
                                     struct radeon_info *info)
{
	*info = ((struct amdgpu_winsys *)rws)->info;
}

struct radeon_winsys *
radv_amdgpu_winsys_create(int fd)
{
	uint32_t drm_major, drm_minor, r;
	amdgpu_device_handle dev;
	struct amdgpu_winsys *ws;
   
	r = amdgpu_device_initialize(fd, &drm_major, &drm_minor, &dev);
	if (r)
		return NULL;

	ws = calloc(1, sizeof(struct amdgpu_winsys));
	if (!ws)
		return NULL;


	ws->dev = dev;
	ws->info.drm_major = drm_major;
	ws->info.drm_minor = drm_minor;
	if (!do_winsys_init(ws, fd))
		goto fail;

	ws->base.query_info = amdgpu_winsys_query_info;
	radv_amdgpu_bo_init_functions(ws);
	radv_amdgpu_cs_init_functions(ws);
	radv_amdgpu_surface_init_functions(ws);
	return &ws->base;
fail:
	return NULL;
}
