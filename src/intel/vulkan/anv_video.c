/*
 * Copyright © 2021 Red Hat
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

#include "anv_private.h"

#include "av1_tables.h"
#include "vk_video/vulkan_video_codecs_common.h"

VkResult
anv_CreateVideoSessionKHR(VkDevice _device,
                           const VkVideoSessionCreateInfoKHR *pCreateInfo,
                           const VkAllocationCallbacks *pAllocator,
                           VkVideoSessionKHR *pVideoSession)
{
   ANV_FROM_HANDLE(anv_device, device, _device);

   struct anv_video_session *vid =
      vk_alloc2(&device->vk.alloc, pAllocator, sizeof(*vid), 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!vid)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   memset(vid, 0, sizeof(struct anv_video_session));

   VkResult result = vk_video_session_init(&device->vk,
                                           &vid->vk,
                                           pCreateInfo);
   if (result != VK_SUCCESS) {
      vk_free2(&device->vk.alloc, pAllocator, vid);
      return result;
   }

   *pVideoSession = anv_video_session_to_handle(vid);
   return VK_SUCCESS;
}

void
anv_DestroyVideoSessionKHR(VkDevice _device,
                           VkVideoSessionKHR _session,
                           const VkAllocationCallbacks *pAllocator)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_video_session, vid, _session);
   if (!_session)
      return;

   vk_object_base_finish(&vid->vk.base);
   vk_free2(&device->vk.alloc, pAllocator, vid);
}

VkResult
anv_CreateVideoSessionParametersKHR(VkDevice _device,
                                     const VkVideoSessionParametersCreateInfoKHR *pCreateInfo,
                                     const VkAllocationCallbacks *pAllocator,
                                     VkVideoSessionParametersKHR *pVideoSessionParameters)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_video_session, vid, pCreateInfo->videoSession);
   ANV_FROM_HANDLE(anv_video_session_params, templ, pCreateInfo->videoSessionParametersTemplate);
   struct anv_video_session_params *params =
      vk_alloc2(&device->vk.alloc, pAllocator, sizeof(*params), 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!params)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   VkResult result = vk_video_session_parameters_init(&device->vk,
                                                      &params->vk,
                                                      &vid->vk,
                                                      templ ? &templ->vk : NULL,
                                                      pCreateInfo);
   if (result != VK_SUCCESS) {
      vk_free2(&device->vk.alloc, pAllocator, params);
      return result;
   }

   *pVideoSessionParameters = anv_video_session_params_to_handle(params);
   return VK_SUCCESS;
}

void
anv_DestroyVideoSessionParametersKHR(VkDevice _device,
                                      VkVideoSessionParametersKHR _params,
                                      const VkAllocationCallbacks *pAllocator)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_video_session_params, params, _params);
   if (!_params)
      return;
   vk_video_session_parameters_finish(&device->vk, &params->vk);
   vk_free2(&device->vk.alloc, pAllocator, params);
}

VkResult
anv_GetPhysicalDeviceVideoCapabilitiesKHR(VkPhysicalDevice physicalDevice,
                                           const VkVideoProfileInfoKHR *pVideoProfile,
                                           VkVideoCapabilitiesKHR *pCapabilities)
{
   pCapabilities->minBitstreamBufferOffsetAlignment = 32;
   pCapabilities->minBitstreamBufferSizeAlignment = 1;
   pCapabilities->pictureAccessGranularity.width = ANV_MB_WIDTH;
   pCapabilities->pictureAccessGranularity.height = ANV_MB_HEIGHT;
   pCapabilities->minCodedExtent.width = ANV_MB_WIDTH;
   pCapabilities->minCodedExtent.height = ANV_MB_HEIGHT;
   pCapabilities->maxCodedExtent.width = 4096;
   pCapabilities->maxCodedExtent.height = 4096;
   pCapabilities->flags = VK_VIDEO_CAPABILITY_SEPARATE_REFERENCE_IMAGES_BIT_KHR;

   struct VkVideoDecodeCapabilitiesKHR *dec_caps = (struct VkVideoDecodeCapabilitiesKHR *)
      vk_find_struct(pCapabilities->pNext, VIDEO_DECODE_CAPABILITIES_KHR);

   switch (pVideoProfile->videoCodecOperation) {
   case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR: {
      struct VkVideoDecodeH264CapabilitiesKHR *ext = (struct VkVideoDecodeH264CapabilitiesKHR *)
         vk_find_struct(pCapabilities->pNext, VIDEO_DECODE_H264_CAPABILITIES_KHR);
      pCapabilities->maxDpbSlots = 17;
      pCapabilities->maxActiveReferencePictures = 16;

      ext->fieldOffsetGranularity.x = 0;
      ext->fieldOffsetGranularity.y = 0;
      ext->maxLevelIdc = 51;
      strcpy(pCapabilities->stdHeaderVersion.extensionName, VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_EXTENSION_NAME);
      pCapabilities->stdHeaderVersion.specVersion = VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_SPEC_VERSION;
      dec_caps->flags = VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR;
      break;
   }
   case VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_MESA: {
      struct VkVideoDecodeAV1CapabilitiesMESA *ext = (struct VkVideoDecodeAV1CapabilitiesMESA *)
         vk_find_struct(pCapabilities->pNext, VIDEO_DECODE_AV1_CAPABILITIES_MESA);
      pCapabilities->maxDpbSlots = 8;
      pCapabilities->maxActiveReferencePictures = 7;
      dec_caps->flags = VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_DISTINCT_BIT_KHR | VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR;
      break;
   }
   default:
      break;
   }
   return VK_SUCCESS;
}

VkResult
anv_GetPhysicalDeviceVideoFormatPropertiesKHR(VkPhysicalDevice physicalDevice,
                                               const VkPhysicalDeviceVideoFormatInfoKHR *pVideoFormatInfo,
                                               uint32_t *pVideoFormatPropertyCount,
                                               VkVideoFormatPropertiesKHR *pVideoFormatProperties)
{
   *pVideoFormatPropertyCount = 1;

   if (!pVideoFormatProperties)
      return VK_SUCCESS;

   pVideoFormatProperties[0].format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
   pVideoFormatProperties[0].imageType = VK_IMAGE_TYPE_2D;
   pVideoFormatProperties[0].imageTiling = VK_IMAGE_TILING_OPTIMAL;
   pVideoFormatProperties[0].imageUsageFlags = pVideoFormatInfo->imageUsage;
   return VK_SUCCESS;
}

static void
get_h264_video_session_mem_reqs(struct anv_video_session *vid,
                                VkVideoSessionMemoryRequirementsKHR *mem_reqs,
                                uint32_t memory_types)
{
   uint32_t width_in_mb = align(vid->vk.max_coded.width, ANV_MB_WIDTH) / ANV_MB_WIDTH;
   /* intra row store is width in macroblocks * 64 */
   mem_reqs[0].memoryBindIndex = ANV_VID_MEM_H264_INTRA_ROW_STORE;
   mem_reqs[0].memoryRequirements.size = width_in_mb * 64;
   mem_reqs[0].memoryRequirements.alignment = 4096;
   mem_reqs[0].memoryRequirements.memoryTypeBits = memory_types;

   /* deblocking filter row store is width in macroblocks * 64 * 4*/
   mem_reqs[1].memoryBindIndex = ANV_VID_MEM_H264_DEBLOCK_FILTER_ROW_STORE;
   mem_reqs[1].memoryRequirements.size = width_in_mb * 64 * 4;
   mem_reqs[1].memoryRequirements.alignment = 4096;
   mem_reqs[1].memoryRequirements.memoryTypeBits = memory_types;

   /* bsd mpc row scratch is width in macroblocks * 64 * 2 */
   mem_reqs[2].memoryBindIndex = ANV_VID_MEM_H264_BSD_MPC_ROW_SCRATCH;
   mem_reqs[2].memoryRequirements.size = width_in_mb * 64 * 2;
   mem_reqs[2].memoryRequirements.alignment = 4096;
   mem_reqs[2].memoryRequirements.memoryTypeBits = memory_types;

   /* mpr row scratch is width in macroblocks * 64 * 2 */
   mem_reqs[3].memoryBindIndex = ANV_VID_MEM_H264_MPR_ROW_SCRATCH;
   mem_reqs[3].memoryRequirements.size = width_in_mb * 64 * 2;
   mem_reqs[3].memoryRequirements.alignment = 4096;
   mem_reqs[3].memoryRequirements.memoryTypeBits = memory_types;
}

static const uint8_t av1_buffer_size[ANV_VID_MEM_AV1_MAX][4] = {
   { 2 ,   4   ,   2   ,    4 }, //bsdLineBuf,
   { 2 ,   4   ,   2   ,    4 }, //bsdTileLineBuf,
   { 2 ,   4   ,   4   ,    8 }, //intraPredLine,
   { 2 ,   4   ,   4   ,    8 }, //intraPredTileLine,
   { 4 ,   8   ,   4   ,    8 }, //spatialMvLineBuf,
   { 4 ,   8   ,   4   ,    8 }, //spatialMvTileLineBuf,
   { 1 ,   1   ,   1   ,    1 }, //lrMetaTileCol,
   { 7 ,   7   ,   7   ,    7 }, //lrTileLineY,
   { 5 ,   5   ,   5   ,    5 }, //lrTileLineU,
   { 5 ,   5   ,   5   ,    5 }, //lrTileLineV,
   { 9 ,   17  ,   11  ,    21 }, //deblockLineYBuf,
   { 3 ,   4   ,   3   ,    5 }, //deblockLineUBuf,
   { 3 ,   4   ,   3   ,    5 }, //deblockLineVBuf,
   { 9 ,   17  ,   11  ,    21 }, //deblockTileLineYBuf,
   { 3 ,   4   ,   3   ,    5 }, //deblockTileLineVBuf,
   { 3 ,   4   ,   3   ,    5 }, //deblockTileLineUBuf,
   { 8 ,   16  ,   10  ,    20 }, //deblockTileColYBuf,
   { 2 ,   4   ,   3   ,    5 }, //deblockTileColUBuf,
   { 2 ,   4   ,   3   ,    5 }, //deblockTileColVBuf,
   { 8 ,   16  ,   10  ,    20 }, //cdefLineBuf,
   { 8 ,   16  ,   10  ,    20 }, //cdefTileLineBuf,
   { 8 ,   16  ,   10  ,    20 }, //cdefTileColBuf,
   { 1 ,   1   ,   1   ,    1 }, //cdefMetaTileLine,
   { 1 ,   1   ,   1   ,    1 }, //cdefMetaTileCol,
   { 1 ,   1   ,   1   ,    1 }, //cdefTopLeftCornerBuf,
   { 22,   44  ,   29  ,    58 }, //superResTileColYBuf,
   { 8 ,   16  ,   10  ,    20 }, //superResTileColUBuf,
   { 8 ,   16  ,   10  ,    20 }, //superResTileColVBuf,
   { 9 ,   17  ,   11  ,    22 }, //lrTileColYBuf,
   { 5 ,   9   ,   6   ,    12 }, //lrTileColUBuf,
   { 5 ,   9   ,   6   ,    12 }, //lrTileColVBuf,
};

static const uint8_t av1_buffer_size_ext[ANV_VID_MEM_AV1_MAX][4] = {
   { 0 ,    0    ,    0    ,    0 }, //bsdLineBuf,
   { 0 ,    0    ,    0    ,    0 }, //bsdTileLineBuf,
   { 0 ,    0    ,    0    ,    0 }, //intraPredLine,
   { 0 ,    0    ,    0    ,    0 }, //intraPredTileLine,
   { 0 ,    0    ,    0    ,    0 }, //spatialMvLineBuf,
   { 0 ,    0    ,    0    ,    0 }, //spatialMvTileLineBuf,
   { 1 ,    1    ,    1    ,    1 }, //lrMetaTileCol,
   { 0 ,    0    ,    0    ,    0 }, //lrTileLineY,
   { 0 ,    0    ,    0    ,    0 }, //lrTileLineU,
   { 0 ,    0    ,    0    ,    0 }, //lrTileLineV,
   { 0 ,    0    ,    0    ,    0 }, //deblockLineYBuf,
   { 0 ,    0    ,    0    ,    0 }, //deblockLineUBuf,
   { 0 ,    0    ,    0    ,    0 }, //deblockLineVBuf,
   { 0 ,    0    ,    0    ,    0 }, //deblockTileLineYBuf,
   { 0 ,    0    ,    0    ,    0 }, //deblockTileLineVBuf,
   { 0 ,    0    ,    0    ,    0 }, //deblockTileLineUBuf,
   { 0 ,    0    ,    0    ,    0 }, //deblockTileColYBuf,
   { 0 ,    0    ,    0    ,    0 }, //deblockTileColUBuf,
   { 0 ,    0    ,    0    ,    0 }, //deblockTileColVBuf,
   { 1 ,    1    ,    2    ,    2 }, //cdefLineBuf,
   { 1 ,    1    ,    2    ,    2 }, //cdefTileLineBuf,
   { 1 ,    1    ,    2    ,    2 }, //cdefTileColBuf,
   { 0 ,    0    ,    0    ,    0 }, //cdefMetaTileLine,
   { 1 ,    1    ,    1    ,    1 }, //cdefMetaTileCol,
   { 0 ,    0    ,    0    ,    0 }, //cdefTopLeftCornerBuf,
   { 22,    44   ,    29   ,    58 }, //superResTileColYBuf,
   { 8 ,    16   ,    10   ,    20 }, //superResTileColUBuf,
   { 8 ,    16   ,    10   ,    20 }, //superResTileColVBuf,
   { 2 ,    2    ,    2    ,    2 }, //lrTileColYBuf,
   { 1 ,    1    ,    1    ,    1 }, //lrTileColUBuf,
   { 1 ,    1    ,    1    ,    1 }, //lrTileColVBuf,
};

static void
get_av1_video_session_mem_reqs(struct anv_video_session *vid,
                               VkVideoSessionMemoryRequirementsKHR *mem_reqs,
                               uint32_t memory_types)
{
   int idx = 0;
   const uint32_t av1_mi_size_log2         = 2;
   const uint32_t av1_max_mib_size_log2    = 5;
   uint32_t width = vid->vk.max_coded.width;
   uint32_t height = vid->vk.max_coded.height;
   uint32_t mi_cols = width  >> av1_mi_size_log2;
   uint32_t mi_rows = height >> av1_mi_size_log2;
   uint32_t width_in_sb = align(mi_cols, (1 << av1_mi_size_log2)) >> av1_mi_size_log2;
   uint32_t height_in_sb = align(mi_rows, (1 << av1_mi_size_log2)) >> av1_mi_size_log2;
   uint32_t max_tile_width_sb = DIV_ROUND_UP(4096, 1 << (av1_max_mib_size_log2 + av1_mi_size_log2));
   uint32_t max_tile_cols = 16; // get the profile to work this out
   /* assume 8-bit 128x128 sb is true, can't know at this point */
   int buf_size_idx = 1;
   for (enum anv_vid_mem_av1_types mem = ANV_VID_MEM_AV1_BITSTREAM_LINE_ROWSTORE;
        mem < ANV_VID_MEM_AV1_MAX; mem++) {
      VkDeviceSize buffer_size = 0;
      mem_reqs[idx].memoryBindIndex = mem;
      mem_reqs[idx].memoryRequirements.alignment = 4096;
      mem_reqs[idx].memoryRequirements.memoryTypeBits = memory_types;

      switch (mem) {
      case ANV_VID_MEM_AV1_BITSTREAM_LINE_ROWSTORE:
      case ANV_VID_MEM_AV1_INTRA_PREDICTION_LINE_ROWSTORE:
      case ANV_VID_MEM_AV1_SPATIAL_MOTION_VECTOR_LINE:
      case ANV_VID_MEM_AV1_DEBLOCKER_FILTER_LINE_Y:
      case ANV_VID_MEM_AV1_DEBLOCKER_FILTER_LINE_U:
      case ANV_VID_MEM_AV1_DEBLOCKER_FILTER_LINE_V:
         buffer_size = max_tile_width_sb * av1_buffer_size[mem][buf_size_idx];
         break;
      case ANV_VID_MEM_AV1_CDEF_FILTER_LINE:
         buffer_size = max_tile_width_sb * av1_buffer_size[mem][buf_size_idx] +
            av1_buffer_size_ext[mem][buf_size_idx];
         break;
      case ANV_VID_MEM_AV1_BITSTREAM_TILE_LINE_ROWSTORE:
      case ANV_VID_MEM_AV1_SPATIAL_MOTION_VECTOR_TILE_LINE:
      case ANV_VID_MEM_AV1_INTRA_PREDICTION_TILE_LINE_ROWSTORE:
      case ANV_VID_MEM_AV1_DEBLOCKER_FILTER_TILE_LINE_Y:
      case ANV_VID_MEM_AV1_DEBLOCKER_FILTER_TILE_LINE_U:
      case ANV_VID_MEM_AV1_DEBLOCKER_FILTER_TILE_LINE_V:
         buffer_size = width_in_sb * av1_buffer_size[mem][buf_size_idx];
         break;

      case ANV_VID_MEM_AV1_LOOP_RESTORATION_FILTER_TILE_LINE_Y:
         buffer_size = max_tile_cols * 7;
         break;
      case ANV_VID_MEM_AV1_LOOP_RESTORATION_FILTER_TILE_LINE_U:
      case ANV_VID_MEM_AV1_LOOP_RESTORATION_FILTER_TILE_LINE_V:
         buffer_size = max_tile_cols * 5;
         break;

      case ANV_VID_MEM_AV1_DEBLOCKER_FILTER_TILE_COLUMN_Y:
      case ANV_VID_MEM_AV1_DEBLOCKER_FILTER_TILE_COLUMN_U:
      case ANV_VID_MEM_AV1_DEBLOCKER_FILTER_TILE_COLUMN_V:
         buffer_size = height_in_sb * av1_buffer_size[mem][buf_size_idx];
         break;
      case ANV_VID_MEM_AV1_CDEF_FILTER_TILE_LINE:
         buffer_size = width_in_sb * av1_buffer_size[mem][buf_size_idx] +
            av1_buffer_size_ext[mem][buf_size_idx];
         break;
      case ANV_VID_MEM_AV1_CDEF_FILTER_META_TILE_LINE:
         buffer_size = max_tile_cols;
         break;
      case ANV_VID_MEM_AV1_CDEF_FILTER_TOP_LEFT_CORNER:
         buffer_size = max_tile_cols * 8;// from profile TODO
         break;
      case ANV_VID_MEM_AV1_CDEF_FILTER_TILE_COLUMN:
      case ANV_VID_MEM_AV1_CDEF_FILTER_META_TILE_COLUMN:
      case ANV_VID_MEM_AV1_SUPER_RES_TILE_COLUMN_Y:
      case ANV_VID_MEM_AV1_SUPER_RES_TILE_COLUMN_U:
      case ANV_VID_MEM_AV1_SUPER_RES_TILE_COLUMN_V:
      case ANV_VID_MEM_AV1_LOOP_RESTORATION_FILTER_TILE_COLUMN_Y:
      case ANV_VID_MEM_AV1_LOOP_RESTORATION_FILTER_TILE_COLUMN_U:
      case ANV_VID_MEM_AV1_LOOP_RESTORATION_FILTER_TILE_COLUMN_V:
      case ANV_VID_MEM_AV1_LOOP_RESTORATION_META_TILE_COLUMN:
         buffer_size = height_in_sb * av1_buffer_size[mem][buf_size_idx] +
            av1_buffer_size_ext[mem][buf_size_idx];
         break;
      case ANV_VID_MEM_AV1_CDF_DEFAULTS_0:
      case ANV_VID_MEM_AV1_CDF_DEFAULTS_1:
      case ANV_VID_MEM_AV1_CDF_DEFAULTS_2:
      case ANV_VID_MEM_AV1_CDF_DEFAULTS_3:
         buffer_size = av1_cdf_max_num_bytes;
         break;
      case ANV_VID_MEM_AV1_DBD_BUFFER:
         buffer_size = 1;
         break;
      default:
         assert(0);
         break;
      }

      switch (mem) {
      case ANV_VID_MEM_AV1_CDF_DEFAULTS_0:
      case ANV_VID_MEM_AV1_CDF_DEFAULTS_1:
      case ANV_VID_MEM_AV1_CDF_DEFAULTS_2:
      case ANV_VID_MEM_AV1_CDF_DEFAULTS_3:
         mem_reqs[idx].memoryRequirements.size = buffer_size;
         break;
      default:
         mem_reqs[idx].memoryRequirements.size = buffer_size * 64;
         break;
      }
      idx++;
   }
}

VkResult
anv_GetVideoSessionMemoryRequirementsKHR(VkDevice _device,
                                         VkVideoSessionKHR videoSession,
                                         uint32_t *pVideoSessionMemoryRequirementsCount,
                                         VkVideoSessionMemoryRequirementsKHR *mem_reqs)
{
   ANV_FROM_HANDLE(anv_device, device, _device);
   ANV_FROM_HANDLE(anv_video_session, vid, videoSession);

   switch (vid->vk.op) {
   case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR:
      *pVideoSessionMemoryRequirementsCount = ANV_VIDEO_MEM_REQS_H264;
      break;
   case VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_MESA:
      *pVideoSessionMemoryRequirementsCount = ANV_VID_MEM_AV1_MAX;
      break;
   default:
      unreachable("unknown codec");
   }
   if (!mem_reqs)
      return VK_SUCCESS;

   uint32_t memory_types = (1ull << device->physical->memory.type_count) - 1;
   switch (vid->vk.op) {
   case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR:
      get_h264_video_session_mem_reqs(vid, mem_reqs, memory_types);
      break;
   case VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_MESA:
      get_av1_video_session_mem_reqs(vid, mem_reqs, memory_types);
      break;
   default:
      unreachable("unknown codec");
   }

   return VK_SUCCESS;
}

VkResult
anv_UpdateVideoSessionParametersKHR(VkDevice _device,
                                     VkVideoSessionParametersKHR _params,
                                     const VkVideoSessionParametersUpdateInfoKHR *pUpdateInfo)
{
   ANV_FROM_HANDLE(anv_video_session_params, params, _params);
   return vk_video_session_parameters_update(&params->vk, pUpdateInfo);
}

static void
copy_bind(struct anv_vid_mem *dst,
          const VkBindVideoSessionMemoryInfoKHR *src)
{
   dst->mem = anv_device_memory_from_handle(src->memory);
   dst->offset = src->memoryOffset;
   dst->size = src->memorySize;
}

VkResult
anv_BindVideoSessionMemoryKHR(VkDevice _device,
                              VkVideoSessionKHR videoSession,
                              uint32_t bind_mem_count,
                              const VkBindVideoSessionMemoryInfoKHR *bind_mem)
{
   ANV_FROM_HANDLE(anv_video_session, vid, videoSession);

   switch (vid->vk.op) {
   case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR:
      assert(bind_mem_count == ANV_VIDEO_MEM_REQS_H264);

      for (unsigned i = 0; i < bind_mem_count; i++) {
         copy_bind(&vid->vid_mem[bind_mem[i].memoryBindIndex], &bind_mem[i]);
      }
      break;
   case VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_MESA:
      for (unsigned i = 0; i < bind_mem_count; i++) {
         copy_bind(&vid->vid_mem[bind_mem[i].memoryBindIndex], &bind_mem[i]);
      }
      break;
   default:
      unreachable("unknown codec");
   }
   return VK_SUCCESS;
}

static void init_single_av1_entry(const struct syntax_element_cdf_table_layout *entry, uint16_t *dst_ptr)
{
   uint16_t entry_count_per_cl = entry->entry_count_per_cl;
   uint16_t entry_count_total = entry->entry_count_total;
   uint16_t start_cl = entry->start_cl;

   const uint16_t *src = entry->init_data;
   uint16_t *dst = dst_ptr + start_cl * 32;
   uint16_t entry_count_left = entry_count_total;

   while (entry_count_left >= entry_count_per_cl) {
      memcpy(dst, src, entry_count_per_cl * sizeof(uint16_t));
      entry_count_left -= entry_count_per_cl;

      src += entry_count_per_cl;
      dst += 32;
   }

   if (entry_count_left > 0)
      memcpy(dst, src, entry_count_left * sizeof(uint16_t));
}

#define INIT_TABLE(x) do {\
   for (unsigned i = 0; i < ARRAY_SIZE((x)); i++) \
      init_single_av1_entry(&(x)[i], dst_ptr); \
   } while (0)

static void init_all_av1_entry(uint16_t *dst_ptr, int index)
{
   INIT_TABLE(av1_cdf_intra_part1);

   switch (index) {
   case 0:
      INIT_TABLE(av1_cdf_intra_coeffs_0);
      break;
   case 1:
      INIT_TABLE(av1_cdf_intra_coeffs_1);
      break;
   case 2:
      INIT_TABLE(av1_cdf_intra_coeffs_2);
      break;
   case 3:
      INIT_TABLE(av1_cdf_intra_coeffs_3);
      break;
   default:
      unreachable("illegal av1 entry\n");
   }
   INIT_TABLE(av1_cdf_intra_part2);
   INIT_TABLE(av1_cdf_inter);
}

void anv_init_av1_cdf_tables(struct anv_device *device,
                             struct anv_video_session *vid)
{
   void *ptr;

   for (unsigned i = 0; i < 4; i++) {
      VkResult result = anv_device_map_bo(device, vid->vid_mem[ANV_VID_MEM_AV1_CDF_DEFAULTS_0 + i].mem->bo,
                                          vid->vid_mem[ANV_VID_MEM_AV1_CDF_DEFAULTS_0 + i].offset,
                                          vid->vid_mem[ANV_VID_MEM_AV1_CDF_DEFAULTS_0 + i].size, 0,
                                          &ptr);
      init_all_av1_entry(ptr, i);
      anv_device_unmap_bo(device, vid->vid_mem[ANV_VID_MEM_AV1_CDF_DEFAULTS_0 + i].mem->bo, ptr,
                          vid->vid_mem[ANV_VID_MEM_AV1_CDF_DEFAULTS_0 + i].size);

   }
}

uint32_t anv_video_get_image_mv_size(struct anv_device *device,
                                   struct anv_image *image,
                                   const struct VkVideoProfileListInfoKHR *profile_list)
{
   uint32_t size = 0;

   for (unsigned i = 0; i < profile_list->profileCount; i++) {
      if (profile_list->pProfiles[i].videoCodecOperation == VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR) {
         unsigned w_mb = DIV_ROUND_UP(image->vk.extent.width, ANV_MB_WIDTH);
         unsigned h_mb = DIV_ROUND_UP(image->vk.extent.height, ANV_MB_HEIGHT);
         size = w_mb * h_mb * 128;
      }

      if (profile_list->pProfiles[i].videoCodecOperation == VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_MESA) {
         const uint32_t av1_mi_size_log2 = 2;
         uint32_t width = image->vk.extent.width;
         uint32_t height = image->vk.extent.height;
         uint32_t mi_cols = width  >> av1_mi_size_log2;
         uint32_t mi_rows = height >> av1_mi_size_log2;
         uint32_t width_in_sb = align(mi_cols, (1 << av1_mi_size_log2)) >> av1_mi_size_log2;
         uint32_t height_in_sb = align(mi_rows, (1 << av1_mi_size_log2)) >> av1_mi_size_log2;
         uint32_t sb_total = width_in_sb * height_in_sb;

         size = sb_total * 16;
      }
   }
   return size;
}
