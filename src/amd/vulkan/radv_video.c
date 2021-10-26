#include "radv_private.h"

#include "ac_vcn_dec_regs.h"

// THIS violates the way vulkan works and send the VCN create/destroy on session
// create/destroy. I don't think this is how things should work, and it changes
// behaviour but doesn't seem to help - DEBUG ONLY
#define SEND_ON_CREATE_HACK

#define NUM_H264_REFS  17
#define FB_BUFFER_OFFSET             0x1000
#define FB_BUFFER_SIZE               2048
#define IT_SCALING_TABLE_SIZE        992
#define VP9_PROBS_TABLE_SIZE         (RDECODE_VP9_PROBS_DATA_SIZE + 256)
#define RDECODE_SESSION_CONTEXT_SIZE (128 * 1024)

static void rvcn_dec_message_create(struct radv_video_session *vid,
				    void *ptr, uint32_t size);
static void rvcn_dec_message_destroy(uint32_t stream_handle,
				     void *ptr, uint32_t size);
static void send_cmd(struct radv_device *device,
		     struct radeon_cmdbuf *cs, unsigned cmd,
                     struct radeon_winsys_bo *bo, uint32_t offset);
/* generate an stream handle */
static unsigned si_vid_alloc_stream_handle()
{
   static unsigned counter = 0;
   unsigned stream_handle = 0;
   unsigned pid = getpid();
   int i;

   for (i = 0; i < 32; ++i)
      stream_handle |= ((pid >> i) & 1) << (31 - i);

   stream_handle ^= ++counter;
   return stream_handle;
}

void
radv_init_physical_device_decoder(struct radv_physical_device *pdevice)
{
   switch (pdevice->rad_info.family) {
   case CHIP_RAVEN:
   case CHIP_RAVEN2:
      pdevice->vid_dec_reg.data0 = RDECODE_VCN1_GPCOM_VCPU_DATA0;
      pdevice->vid_dec_reg.data1 = RDECODE_VCN1_GPCOM_VCPU_DATA1;
      pdevice->vid_dec_reg.cmd = RDECODE_VCN1_GPCOM_VCPU_CMD;
      pdevice->vid_dec_reg.cntl = RDECODE_VCN1_ENGINE_CNTL;
      break;
   case CHIP_NAVI10:
   case CHIP_NAVI12:
   case CHIP_NAVI14:
   case CHIP_RENOIR:
      pdevice->vid_dec_reg.data0 = RDECODE_VCN2_GPCOM_VCPU_DATA0;
      pdevice->vid_dec_reg.data1 = RDECODE_VCN2_GPCOM_VCPU_DATA1;
      pdevice->vid_dec_reg.cmd = RDECODE_VCN2_GPCOM_VCPU_CMD;
      pdevice->vid_dec_reg.cntl = RDECODE_VCN2_ENGINE_CNTL;
      break;
   case CHIP_ARCTURUS:
   case CHIP_ALDEBARAN:
   case CHIP_SIENNA_CICHLID:
   case CHIP_NAVY_FLOUNDER:
   case CHIP_DIMGREY_CAVEFISH:
   case CHIP_BEIGE_GOBY:
   case CHIP_VANGOGH:
   case CHIP_YELLOW_CARP:
      pdevice->vid_dec_reg.data0 = RDECODE_VCN2_5_GPCOM_VCPU_DATA0;
      pdevice->vid_dec_reg.data1 = RDECODE_VCN2_5_GPCOM_VCPU_DATA1;
      pdevice->vid_dec_reg.cmd = RDECODE_VCN2_5_GPCOM_VCPU_CMD;
      pdevice->vid_dec_reg.cntl = RDECODE_VCN2_5_ENGINE_CNTL;
      break;
   default:
      break;
   }
}

static bool have_it(struct radv_video_session *vid)
{
   return vid->stream_type == RDECODE_CODEC_H264_PERF || vid->stream_type == RDECODE_CODEC_H265;
}

/* do the codec needs an probs buffer? */
static bool have_probs(struct radv_video_session *vid)
{
   return (vid->stream_type == RDECODE_CODEC_VP9 || vid->stream_type == RDECODE_CODEC_AV1);
}

static inline uint32_t
u_get_h264_level(uint32_t width, uint32_t height, uint32_t *max_reference)
{
   uint32_t max_dpb_mbs;

   width = align(width, 16);
   height = align(height, 16);

   /* Max references will be used for caculation of number of DPB buffers
      in the UVD driver, limitation of max references is 16. Some client
      like mpv application for VA-API, it requires references more than that,
      so we have to set max of references to 16 here. */
   *max_reference = MIN2(*max_reference, 16);
   max_dpb_mbs = (width / 16) * (height / 16) * *max_reference;

   /* The calculation is based on "Decoded picture buffering" section
      from http://en.wikipedia.org/wiki/H.264/MPEG-4_AVC */
   if (max_dpb_mbs <= 8100)
      return 30;
   else if (max_dpb_mbs <= 18000)
      return 31;
   else if (max_dpb_mbs <= 20480)
      return 32;
   else if (max_dpb_mbs <= 32768)
      return 41;
   else if (max_dpb_mbs <= 34816)
      return 42;
   else if (max_dpb_mbs <= 110400)
      return 50;
   else if (max_dpb_mbs <= 184320)
      return 51;
   else
      return 52;
}

static unsigned calc_ctx_size_h264_perf(struct radv_video_session *vid)
{
   unsigned width_in_mb, height_in_mb, ctx_size;
   unsigned width = align(vid->max_coded.width, VL_MACROBLOCK_WIDTH);
   unsigned height = align(vid->max_coded.height, VL_MACROBLOCK_HEIGHT);

   unsigned max_references = vid->max_ref_pic_slots + 1;

   // picture width & height in 16 pixel units
   width_in_mb = width / VL_MACROBLOCK_WIDTH;
   height_in_mb = align(height / VL_MACROBLOCK_HEIGHT, 2);

   unsigned fs_in_mb = width_in_mb * height_in_mb;
   unsigned num_dpb_buffer;
   switch (vid->level) {
   case 30:
      num_dpb_buffer = 8100 / fs_in_mb;
      break;
   case 31:
      num_dpb_buffer = 18000 / fs_in_mb;
      break;
   case 32:
      num_dpb_buffer = 20480 / fs_in_mb;
      break;
   case 41:
      num_dpb_buffer = 32768 / fs_in_mb;
      break;
   case 42:
      num_dpb_buffer = 34816 / fs_in_mb;
      break;
   case 50:
      num_dpb_buffer = 110400 / fs_in_mb;
      break;
   case 51:
      num_dpb_buffer = 184320 / fs_in_mb;
      break;
   default:
      num_dpb_buffer = 184320 / fs_in_mb;
      break;
   }
   num_dpb_buffer++;
   max_references = MAX2(MIN2(NUM_H264_REFS, num_dpb_buffer), max_references);
   ctx_size = max_references * align(width_in_mb * height_in_mb * 192, 256);

   return ctx_size;
}

static unsigned calc_ctx_size_h265_main(struct radv_video_session *vid)
{
   unsigned width = align(vid->max_coded.width, VL_MACROBLOCK_WIDTH);
   unsigned height = align(vid->max_coded.height, VL_MACROBLOCK_HEIGHT);

   unsigned max_references = vid->max_ref_pic_slots + 1;

   if (vid->max_coded.width * vid->max_coded.height >= 4096 * 2000)
      max_references = MAX2(max_references, 8);
   else
      max_references = MAX2(max_references, 17);

   width = align(width, 16);
   height = align(height, 16);
   return ((width + 255) / 16) * ((height + 255) / 16) * 16 * max_references + 52 * 1024;
}

#if 0
static unsigned calc_ctx_size_h265_main10(struct radv_video_session *vid)
{
   unsigned log2_ctb_size, width_in_ctb, height_in_ctb, num_16x16_block_per_ctb;
   unsigned context_buffer_size_per_ctb_row, cm_buffer_size, max_mb_address, db_left_tile_pxl_size;
   unsigned db_left_tile_ctx_size = 4096 / 16 * (32 + 16 * 4);

   unsigned width = align(vid->max_coded.width, VL_MACROBLOCK_WIDTH);
   unsigned height = align(vid->max_coded.height, VL_MACROBLOCK_HEIGHT);
   unsigned coeff_10bit = 2;

   unsigned max_references = vid->max_ref_pic_slots + 1;

   if (vid->max_coded.width * vid->max_coded.height >= 4096 * 2000)
      max_references = MAX2(max_references, 8);
   else
      max_references = MAX2(max_references, 17);

   log2_ctb_size = 6;//TODO
      //   log2_ctb_size = pic->pps->sps->log2_min_luma_coding_block_size_minus3 + 3 +
      //                   pic->pps->sps->log2_diff_max_min_luma_coding_block_size;

   width_in_ctb = (width + ((1 << log2_ctb_size) - 1)) >> log2_ctb_size;
   height_in_ctb = (height + ((1 << log2_ctb_size) - 1)) >> log2_ctb_size;

   num_16x16_block_per_ctb = ((1 << log2_ctb_size) >> 4) * ((1 << log2_ctb_size) >> 4);
   context_buffer_size_per_ctb_row = align(width_in_ctb * num_16x16_block_per_ctb * 16, 256);
   max_mb_address = (unsigned)ceil(height * 8 / 2048.0);

   cm_buffer_size = max_references * context_buffer_size_per_ctb_row * height_in_ctb;
   db_left_tile_pxl_size = coeff_10bit * (max_mb_address * 2 * 2048 + 1024);

   return cm_buffer_size + db_left_tile_ctx_size + db_left_tile_pxl_size;
}
#endif

/* calculate size of reference picture buffer */
static unsigned calc_dpb_size(struct radv_video_session *vid)
{
   unsigned width_in_mb, height_in_mb, image_size, dpb_size;

   // always align them to MB size for dpb calculation
   unsigned width = align(vid->max_coded.width, VL_MACROBLOCK_WIDTH);
   unsigned height = align(vid->max_coded.height, VL_MACROBLOCK_HEIGHT);

   // always one more for currently decoded picture
   unsigned max_references = vid->max_ref_pic_slots + 1;

   // aligned size of a single frame
   image_size = align(width, 32) * height;
   image_size += image_size / 2;
   image_size = align(image_size, 1024);

   // picture width & height in 16 pixel units
   width_in_mb = width / VL_MACROBLOCK_WIDTH;
   height_in_mb = align(height / VL_MACROBLOCK_HEIGHT, 2);

   // H264 MPEG AVC only.
   {
      unsigned fs_in_mb = width_in_mb * height_in_mb;
      unsigned num_dpb_buffer;

      switch (vid->level) {
      case 30:
         num_dpb_buffer = 8100 / fs_in_mb;
         break;
      case 31:
         num_dpb_buffer = 18000 / fs_in_mb;
         break;
      case 32:
         num_dpb_buffer = 20480 / fs_in_mb;
         break;
      case 41:
         num_dpb_buffer = 32768 / fs_in_mb;
         break;
      case 42:
         num_dpb_buffer = 34816 / fs_in_mb;
         break;
      case 50:
         num_dpb_buffer = 110400 / fs_in_mb;
         break;
      case 51:
         num_dpb_buffer = 184320 / fs_in_mb;
         break;
      default:
         num_dpb_buffer = 184320 / fs_in_mb;
         break;
      }
      num_dpb_buffer++;
      max_references = MAX2(MIN2(NUM_H264_REFS, num_dpb_buffer), max_references);
      dpb_size = image_size * max_references;
   }
   return dpb_size;
}

#define MSG_CREATE 0
#define MSG_DESTROY 1
static void send_single_message_wait(struct radv_device *device, struct radv_video_session *vid,
				     int msg)
{
   struct radeon_winsys_bo *bo = NULL;
   VkResult result = device->ws->buffer_create(device->ws, 4096, 4096, device->ws->cs_domain(device->ws),
					       RADEON_FLAG_CPU_ACCESS | RADEON_FLAG_GTT_WC,
					       RADV_BO_PRIORITY_UPLOAD_BUFFER, 0, &bo);
   struct radeon_cmdbuf *cs = device->ws->cs_create(device->ws, RING_VCN_DEC);

   void *ptr = device->ws->buffer_map(bo);

   uint32_t msg_size = sizeof(rvcn_dec_message_header_t);

   if (msg == MSG_CREATE) {
     msg_size += sizeof(rvcn_dec_message_create_t);
     rvcn_dec_message_create(vid, ptr, msg_size);
   } else
     rvcn_dec_message_destroy(vid->stream_handle, ptr, msg_size);
   device->ws->buffer_unmap(bo);
   //   send_cmd(device, cs, RDECODE_CMD_SESSION_CONTEXT_BUFFER, vid->sessionctx.mem->bo, vid->sessionctx.offset);
   send_cmd(device, cs, RDECODE_CMD_MSG_BUFFER, bo, 0);
   radv_cs_add_buffer(device->ws, cs, bo);

   device->ws->cs_finalize(cs);

   struct radv_queue *queue = device->queues[RADV_QUEUE_VIDEO_DEC];
   radv_queue_internal_submit(queue, cs);


   queue->device->ws->ctx_wait_idle(queue->hw_ctx, radv_queue_family_to_ring(queue->vk.queue_family_index),
				    queue->vk.index_in_family);

   device->ws->cs_destroy(cs);

   device->ws->buffer_destroy(device->ws, bo);

}
VkResult
radv_CreateVideoSessionKHR(VkDevice _device,
                           const VkVideoSessionCreateInfoKHR *pCreateInfo,
                           const VkAllocationCallbacks *pAllocator,
                           VkVideoSessionKHR *pVideoSession)
{
   RADV_FROM_HANDLE(radv_device, device, _device);

   struct radv_video_session *vid =
      vk_alloc2(&device->vk.alloc, pAllocator, sizeof(*vid), 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!vid)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);


   vk_object_base_init(&device->vk, &vid->base, VK_OBJECT_TYPE_VIDEO_SESSION_KHR);

   vid->format = pCreateInfo->pictureFormat;
   vid->max_coded = pCreateInfo->maxCodedExtent;
   vid->ref_format = pCreateInfo->referencePicturesFormat;
   vid->max_ref_pic_slots = pCreateInfo->maxReferencePicturesSlotsCount;
   vid->max_ref_pic_active = pCreateInfo->maxReferencePicturesActiveCount;

   vid->op = pCreateInfo->pVideoProfile->videoCodecOperation;
   vid->interlaced = false;
   const struct VkVideoDecodeH264SessionCreateInfoEXT *h264_create =
      vk_find_struct_const(pCreateInfo->pNext, VIDEO_DECODE_H264_SESSION_CREATE_INFO_EXT);

   switch (vid->op) {
   case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_EXT:
      assert(h264_create);
      //      const struct VkVideoDecodeH264ProfileEXT *h264_profile =
      //vk_find_struct_const(pCreateInfo->pVideoProfile->pNext, VIDEO_DECODE_H264_PROFILE_EXT);
      vid->stream_type = RDECODE_CODEC_H264_PERF;
      uint32_t max_ref = vid->max_ref_pic_slots;
      vid->level = u_get_h264_level(pCreateInfo->maxCodedExtent.width,
                                    pCreateInfo->maxCodedExtent.height, &max_ref);
      break;
   case VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_EXT:
      vid->stream_type = RDECODE_CODEC_H265;
      break;
   default:
      return VK_ERROR_FEATURE_NOT_PRESENT;
   }

   vid->stream_handle = si_vid_alloc_stream_handle();
   vid->dbg_frame_cnt = 0;

   if (device->physical_device->rad_info.family >= CHIP_SIENNA_CICHLID &&
       (vid->stream_type == RDECODE_CODEC_VP9 || vid->stream_type == RDECODE_CODEC_AV1))
      vid->dpb_type = DPB_DYNAMIC_TIER_2;
   else if (device->physical_device->rad_info.family <= CHIP_NAVI14 && vid->stream_type == RDECODE_CODEC_VP9)
      vid->dpb_type = DPB_DYNAMIC_TIER_1;
   else
      vid->dpb_type = DPB_MAX_RES;
   vid->db_alignment = (device->physical_device->rad_info.family >= CHIP_RENOIR &&
                        vid->max_coded.width > 32 && 0) ? 64 : 32;

   vid->dpb_size = calc_dpb_size(vid);

#ifdef SEND_ON_CREATE_HACK
   send_single_message_wait(device, vid, MSG_CREATE);
#endif
   *pVideoSession = radv_video_session_to_handle(vid);
   return VK_SUCCESS;
}

void
radv_DestroyVideoSessionKHR(VkDevice _device,
                            VkVideoSessionKHR _session,
                            const VkAllocationCallbacks *pAllocator)
{
   RADV_FROM_HANDLE(radv_device, device, _device);
   RADV_FROM_HANDLE(radv_video_session, vid, _session);
   if (!_session)
      return;

#ifdef SEND_ON_CREATE_HACK
   send_single_message_wait(device, vid, MSG_DESTROY);
#endif
   vk_object_base_finish(&vid->base);
   vk_free2(&device->vk.alloc, pAllocator, vid);
}


VkResult
radv_CreateVideoSessionParametersKHR(VkDevice _device,
                                     const VkVideoSessionParametersCreateInfoKHR *pCreateInfo,
                                     const VkAllocationCallbacks *pAllocator,
                                     VkVideoSessionParametersKHR *pVideoSessionParameters)
{
   RADV_FROM_HANDLE(radv_device, device, _device);
   RADV_FROM_HANDLE(radv_video_session, vid, pCreateInfo->videoSession);
   struct radv_video_session_params *params =
      vk_alloc2(&device->vk.alloc, pAllocator, sizeof(*params), 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!params)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &params->base, VK_OBJECT_TYPE_VIDEO_SESSION_PARAMETERS_KHR);

   params->op = vid->op;
   switch (vid->op) {
   case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_EXT: {
      const struct VkVideoDecodeH264SessionParametersCreateInfoEXT *h264_create =
         vk_find_struct_const(pCreateInfo->pNext, VIDEO_DECODE_H264_SESSION_PARAMETERS_CREATE_INFO_EXT);

      params->h264_dec.max_sps_std_count = h264_create->maxSpsStdCount;
      params->h264_dec.max_pps_std_count = h264_create->maxPpsStdCount;

      params->h264_dec.sps_std_count = h264_create->pParametersAddInfo->spsStdCount;
      params->h264_dec.pps_std_count = h264_create->pParametersAddInfo->ppsStdCount;

      uint32_t sps_size = params->h264_dec.max_sps_std_count * sizeof(StdVideoH264SequenceParameterSet);
      params->h264_dec.sps_std = malloc(sps_size);
      memcpy(params->h264_dec.sps_std, h264_create->pParametersAddInfo->pSpsStd,
             params->h264_dec.sps_std_count * sizeof(StdVideoH264SequenceParameterSet));

      uint32_t pps_size = params->h264_dec.max_pps_std_count * sizeof(StdVideoH264PictureParameterSet);
      params->h264_dec.pps_std = malloc(pps_size);
      memcpy(params->h264_dec.pps_std, h264_create->pParametersAddInfo->pPpsStd,
             params->h264_dec.pps_std_count * sizeof(StdVideoH264PictureParameterSet));
      break;
   }
   case VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_EXT: {
      const struct VkVideoDecodeH265SessionParametersCreateInfoEXT *h265_create =
         vk_find_struct_const(pCreateInfo->pNext, VIDEO_DECODE_H265_SESSION_PARAMETERS_CREATE_INFO_EXT);

      params->h265_dec.max_sps_std_count = h265_create->maxSpsStdCount;
      params->h265_dec.max_pps_std_count = h265_create->maxPpsStdCount;

      params->h265_dec.sps_std_count = h265_create->pParametersAddInfo->spsStdCount;
      params->h265_dec.pps_std_count = h265_create->pParametersAddInfo->ppsStdCount;

      uint32_t sps_size = params->h265_dec.max_sps_std_count * sizeof(StdVideoH265SequenceParameterSet);
      params->h265_dec.sps_std = malloc(sps_size);
      memcpy(params->h265_dec.sps_std, h265_create->pParametersAddInfo->pSpsStd,
             params->h265_dec.sps_std_count * sizeof(StdVideoH265SequenceParameterSet));

      uint32_t pps_size = params->h265_dec.max_pps_std_count * sizeof(StdVideoH265PictureParameterSet);
      params->h265_dec.pps_std = malloc(pps_size);
      memcpy(params->h265_dec.pps_std, h265_create->pParametersAddInfo->pPpsStd,
             params->h265_dec.pps_std_count * sizeof(StdVideoH265PictureParameterSet));
      break;
   }
   default:
      break;
   }

   *pVideoSessionParameters = radv_video_session_params_to_handle(params);
   return VK_SUCCESS;
}

void
radv_DestroyVideoSessionParametersKHR(VkDevice _device,
                                      VkVideoSessionParametersKHR _params,
                                      const VkAllocationCallbacks *pAllocator)
{
   RADV_FROM_HANDLE(radv_device, device, _device);
   RADV_FROM_HANDLE(radv_video_session_params, params, _params);
   if (!_params)
      return;

   switch (params->op) {
   case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_EXT:
      free(params->h264_dec.sps_std);
      free(params->h264_dec.pps_std);
      break;
   case VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_EXT:
      free(params->h265_dec.sps_std);
      free(params->h265_dec.pps_std);
      break;
   default:
      break;
   }
   vk_object_base_finish(&params->base);
   vk_free2(&device->vk.alloc, pAllocator, params);
}

VkResult
radv_GetPhysicalDeviceVideoCapabilitiesKHR(VkPhysicalDevice physicalDevice,
                                           const VkVideoProfileKHR *pVideoProfile,
                                           VkVideoCapabilitiesKHR *pCapabilities)
{
   RADV_FROM_HANDLE(radv_physical_device, pdevice, physicalDevice);
   struct video_codec_cap *cap = NULL;

   switch (pVideoProfile->videoCodecOperation) {
   case VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_EXT:
      cap = &pdevice->rad_info.enc_caps.codec_info[RADV_VIDEO_FORMAT_MPEG4_AVC];
      break;
   case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_EXT:
      cap = &pdevice->rad_info.dec_caps.codec_info[RADV_VIDEO_FORMAT_MPEG4_AVC];
      break;
   case VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_EXT:
      cap = &pdevice->rad_info.dec_caps.codec_info[RADV_VIDEO_FORMAT_HEVC];
      break;
   default:
      break;
   }

   if (cap && !cap->valid)
      cap = NULL;

   pCapabilities->capabilityFlags = 0;
   pCapabilities->minBitstreamBufferOffsetAlignment = 128;
   pCapabilities->minBitstreamBufferSizeAlignment = 128;
   pCapabilities->videoPictureExtentGranularity.width = VL_MACROBLOCK_WIDTH;
   pCapabilities->videoPictureExtentGranularity.height = VL_MACROBLOCK_HEIGHT;
   pCapabilities->minExtent.width = VL_MACROBLOCK_WIDTH;
   pCapabilities->minExtent.height = VL_MACROBLOCK_HEIGHT;

   if (cap) {
      pCapabilities->maxExtent.width = cap->max_width;
      pCapabilities->maxExtent.height = cap->max_height;
   } else {
      switch (pVideoProfile->videoCodecOperation) {
      case VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_EXT:
         pCapabilities->maxExtent.width = (pdevice->rad_info.family < CHIP_TONGA) ? 2048 : 4096;
         pCapabilities->maxExtent.height = (pdevice->rad_info.family < CHIP_TONGA) ? 1152 : 2304;
         break;
      case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_EXT:
         pCapabilities->maxExtent.width = (pdevice->rad_info.family < CHIP_TONGA) ? 2048 : 4096;
         pCapabilities->maxExtent.height = (pdevice->rad_info.family < CHIP_TONGA) ? 1152 : 4096;
         break;
      case VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_EXT:
         pCapabilities->maxExtent.width = (pdevice->rad_info.family < CHIP_RENOIR) ?
            ((pdevice->rad_info.family < CHIP_TONGA) ? 2048 : 4096) : 8192;
         pCapabilities->maxExtent.height = (pdevice->rad_info.family < CHIP_RENOIR) ?
            ((pdevice->rad_info.family < CHIP_TONGA) ? 1152 : 4096) : 4352;
         break;
      default:
         break;
      }
   }

   // TODO
   return VK_SUCCESS;
}

VkResult
radv_GetPhysicalDeviceVideoFormatPropertiesKHR(VkPhysicalDevice physicalDevice,
                                               const VkPhysicalDeviceVideoFormatInfoKHR *pVideoFormatInfo,
                                               uint32_t *pVideoFormatPropertyCount,
                                               VkVideoFormatPropertiesKHR *pVideoFormatProperties)
{
   *pVideoFormatPropertyCount = 1;

   if (!pVideoFormatProperties)
      return VK_SUCCESS;

   pVideoFormatProperties[0].format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
   return VK_SUCCESS;
}

VkResult
radv_GetVideoSessionMemoryRequirementsKHR(VkDevice _device,
                                          VkVideoSessionKHR videoSession,
                                          uint32_t *pVideoSessionMemoryRequirementsCount,
                                          VkVideoGetMemoryPropertiesKHR *pVideoSessionMemoryRequirements)
{
   RADV_FROM_HANDLE(radv_device, device, _device);
   RADV_FROM_HANDLE(radv_video_session, vid, videoSession);
   uint32_t memory_type_bits = (1u << device->physical_device->memory_properties.memoryTypeCount) - 1;
   uint32_t num_memory_reqs = 2;
   int idx = 0;

   if (vid->stream_type == RDECODE_CODEC_H264_PERF || vid->stream_type == RDECODE_CODEC_H265)
      num_memory_reqs++;

   *pVideoSessionMemoryRequirementsCount = num_memory_reqs;

   if (!pVideoSessionMemoryRequirements)
      return VK_SUCCESS;

   /* 1 buffer for session context */
   pVideoSessionMemoryRequirements[idx].pMemoryRequirements->memoryRequirements.size = RDECODE_SESSION_CONTEXT_SIZE;
   pVideoSessionMemoryRequirements[idx].pMemoryRequirements->memoryRequirements.alignment = 0;
   pVideoSessionMemoryRequirements[idx].pMemoryRequirements->memoryRequirements.memoryTypeBits = memory_type_bits;

   idx++;
   /* internal DPB? */
   pVideoSessionMemoryRequirements[idx].pMemoryRequirements->memoryRequirements.size = vid->dpb_size;
   pVideoSessionMemoryRequirements[idx].pMemoryRequirements->memoryRequirements.alignment = 0;
   pVideoSessionMemoryRequirements[idx].pMemoryRequirements->memoryRequirements.memoryTypeBits = memory_type_bits;

   idx++;
   if (vid->stream_type == RDECODE_CODEC_H264_PERF) {
      pVideoSessionMemoryRequirements[idx].pMemoryRequirements->memoryRequirements.size = align(calc_ctx_size_h264_perf(vid), 4096);
      pVideoSessionMemoryRequirements[idx].pMemoryRequirements->memoryRequirements.alignment = 0;
      pVideoSessionMemoryRequirements[idx].pMemoryRequirements->memoryRequirements.memoryTypeBits = memory_type_bits;
   }
   if (vid->stream_type == RDECODE_CODEC_H265) {
      pVideoSessionMemoryRequirements[idx].pMemoryRequirements->memoryRequirements.size = align(calc_ctx_size_h265_main(vid), 4096);
      pVideoSessionMemoryRequirements[idx].pMemoryRequirements->memoryRequirements.alignment = 0;
      pVideoSessionMemoryRequirements[idx].pMemoryRequirements->memoryRequirements.memoryTypeBits = memory_type_bits;
   }
   return VK_SUCCESS;
}

VkResult
radv_UpdateVideoSessionParametersKHR(VkDevice _device,
                                     VkVideoSessionParametersKHR videoSessionParameters,
                                     const VkVideoSessionParametersUpdateInfoKHR *pUpdateInfo)
{
   RADV_FROM_HANDLE(radv_video_session_params, params, videoSessionParameters);

   switch (params->op) {
   case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_EXT:
      const struct VkVideoDecodeH264SessionParametersAddInfoEXT *h264_add =
         vk_find_struct_const(pUpdateInfo->pNext, VIDEO_DECODE_H264_SESSION_PARAMETERS_CREATE_INFO_EXT);

      params->h264_dec.sps_std_count = h264_add->spsStdCount;
      params->h264_dec.pps_std_count = h264_add->ppsStdCount;

      memcpy(params->h264_dec.sps_std, h264_add->pSpsStd,
             params->h264_dec.sps_std_count * sizeof(StdVideoH264SequenceParameterSet));

      memcpy(params->h264_dec.pps_std, h264_add->pPpsStd,
             params->h264_dec.pps_std_count * sizeof(StdVideoH264PictureParameterSet));

      break;

   case VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_EXT:
      const struct VkVideoDecodeH265SessionParametersAddInfoEXT *h265_add =
         vk_find_struct_const(pUpdateInfo->pNext, VIDEO_DECODE_H265_SESSION_PARAMETERS_CREATE_INFO_EXT);

      params->h265_dec.sps_std_count = h265_add->spsStdCount;
      params->h265_dec.pps_std_count = h265_add->ppsStdCount;

      memcpy(params->h265_dec.sps_std, h265_add->pSpsStd,
             params->h265_dec.sps_std_count * sizeof(StdVideoH265SequenceParameterSet));

      memcpy(params->h265_dec.pps_std, h265_add->pPpsStd,
             params->h265_dec.pps_std_count * sizeof(StdVideoH265PictureParameterSet));

      break;

   default:
      break;
   }

   return VK_SUCCESS;
}

static void
copy_bind(struct radv_vid_mem *dst,
          const VkVideoBindMemoryKHR *src)
{
   dst->bind_index = src->memoryBindIndex;
   dst->mem = radv_device_memory_from_handle(src->memory);
   dst->offset = src->memoryOffset;
   dst->size = src->memorySize;
}

VkResult
radv_BindVideoSessionMemoryKHR(VkDevice _device,
                               VkVideoSessionKHR videoSession,
                               uint32_t videoSessionBindMemoryCount,
                               const VkVideoBindMemoryKHR *pVideoSessionBindMemories)
{
   RADV_FROM_HANDLE(radv_video_session, vid, videoSession);

   assert(videoSessionBindMemoryCount >= 2);
   copy_bind(&vid->sessionctx, &pVideoSessionBindMemories[0]);

   copy_bind(&vid->dpb, &pVideoSessionBindMemories[1]);

   if (videoSessionBindMemoryCount == 3)
      copy_bind(&vid->ctx, &pVideoSessionBindMemories[2]);
   return VK_SUCCESS;
}

/* add a new set register command to the IB */
static void set_reg(struct radeon_cmdbuf *cs, unsigned reg, uint32_t val)
{
   radeon_emit(cs, RDECODE_PKT0(reg >> 2, 0));
   radeon_emit(cs, val);
}

static void send_cmd(struct radv_device *device,
                     struct radeon_cmdbuf *cs, unsigned cmd,
                     struct radeon_winsys_bo *bo, uint32_t offset)
{
   struct radv_physical_device *pdev = device->physical_device;
   uint64_t addr;

   radv_cs_add_buffer(device->ws, cs, bo);
   addr = radv_buffer_get_va(bo);
   addr += offset;
   set_reg(cs, pdev->vid_dec_reg.data0, addr);
   set_reg(cs, pdev->vid_dec_reg.data1, addr >> 32);
   set_reg(cs, pdev->vid_dec_reg.cmd, cmd << 1);
}

static void rvcn_dec_message_create(struct radv_video_session *vid,
                                    void *ptr, uint32_t size)
{
   rvcn_dec_message_header_t *header = ptr;
   rvcn_dec_message_create_t *create = (void *)((char *)ptr + sizeof(rvcn_dec_message_header_t));

   memset(ptr, 0, size);
   header->header_size = sizeof(rvcn_dec_message_header_t);
   header->total_size = size;
   header->num_buffers = 1;
   header->msg_type = RDECODE_MSG_CREATE;
   header->stream_handle = vid->stream_handle;
   header->status_report_feedback_number = 0;

   header->index[0].message_id = RDECODE_MESSAGE_CREATE;
   header->index[0].offset = sizeof(rvcn_dec_message_header_t);
   header->index[0].size = sizeof(rvcn_dec_message_create_t);
   header->index[0].filled = 0;

   create->stream_type = vid->stream_type;
   create->session_flags = 0;
   create->width_in_samples = vid->max_coded.width;
   create->height_in_samples = vid->max_coded.height;
}

static void rvcn_dec_message_destroy(uint32_t stream_handle,
                                     void *ptr, uint32_t size)
{
   rvcn_dec_message_header_t *header = ptr;

   memset(ptr, 0, sizeof(rvcn_dec_message_header_t));
   header->header_size = sizeof(rvcn_dec_message_header_t);
   header->total_size = sizeof(rvcn_dec_message_header_t) - sizeof(rvcn_dec_message_index_t);
   header->num_buffers = 0;
   header->msg_type = RDECODE_MSG_DESTROY;
   header->stream_handle = stream_handle;
   header->status_report_feedback_number = 0;
}

static void rvcn_dec_message_feedback(void *ptr)
{
   rvcn_dec_feedback_header_t *header = (void *)ptr;

   header->header_size = sizeof(rvcn_dec_feedback_header_t);
   header->total_size = sizeof(rvcn_dec_feedback_header_t);
   header->num_buffers = 0;
}

static rvcn_dec_message_avc_t get_h264_msg(struct radv_video_session *vid,
                                           struct radv_video_session_params *params,
                                           const struct VkVideoDecodeInfoKHR *frame_info,
                                           void *it_ptr)
{
   rvcn_dec_message_avc_t result;
   const struct VkVideoDecodeH264PictureInfoEXT *h264_pic_info =
      vk_find_struct_const(frame_info->pNext, VIDEO_DECODE_H264_PICTURE_INFO_EXT);

   memset(&result, 0, sizeof(result));

   assert(params->h264_dec.sps_std_count > 0);
   const StdVideoH264SequenceParameterSet *sps = &params->h264_dec.sps_std[h264_pic_info->pStdPictureInfo->seq_parameter_set_id];
   switch (sps->profile_idc) {
   case std_video_h264_profile_idc_baseline:
      result.profile = RDECODE_H264_PROFILE_BASELINE;
      break;
   case std_video_h264_profile_idc_main:
      result.profile = RDECODE_H264_PROFILE_MAIN;
      break;
   case std_video_h264_profile_idc_high:
      result.profile = RDECODE_H264_PROFILE_HIGH;
      break;
   default:
      fprintf(stderr, "UNSUPPORTED CODEC %d\n", sps->profile_idc);
      result.profile= RDECODE_H264_PROFILE_MAIN;
      break;
   }

   result.level = sps->level_idc;

   result.sps_info_flags = 0;

   result.sps_info_flags |= sps->flags.direct_8x8_inference_flag << 0;
   result.sps_info_flags |= sps->flags.mb_adaptive_frame_field_flag << 1;
   result.sps_info_flags |= sps->flags.frame_mbs_only_flag << 2;
   result.sps_info_flags |= sps->flags.delta_pic_order_always_zero_flag << 3;
   result.sps_info_flags |= 1 << RDECODE_SPS_INFO_H264_EXTENSION_SUPPORT_FLAG_SHIFT;

   result.bit_depth_luma_minus8 = sps->bit_depth_luma_minus8;
   result.bit_depth_chroma_minus8 = sps->bit_depth_chroma_minus8;
   result.log2_max_frame_num_minus4 = sps->log2_max_frame_num_minus4;
   result.pic_order_cnt_type = sps->pic_order_cnt_type;
   result.log2_max_pic_order_cnt_lsb_minus4 = sps->log2_max_pic_order_cnt_lsb_minus4;

   result.chroma_format = sps->chroma_format_idc;

   const StdVideoH264PictureParameterSet *pps = &params->h264_dec.pps_std[h264_pic_info->pStdPictureInfo->pic_parameter_set_id];
   result.pps_info_flags = 0;
   result.pps_info_flags |= pps->flags.transform_8x8_mode_flag << 0;
   result.pps_info_flags |= pps->flags.redundant_pic_cnt_present_flag << 1;
   result.pps_info_flags |= pps->flags.constrained_intra_pred_flag << 2;
   result.pps_info_flags |= pps->flags.deblocking_filter_control_present_flag << 3;
   result.pps_info_flags |= pps->flags.weighted_bipred_idc_flag << 4;
   result.pps_info_flags |= pps->flags.weighted_pred_flag << 6;
   result.pps_info_flags |= pps->flags.pic_order_present_flag << 7;
   result.pps_info_flags |= pps->flags.entropy_coding_mode_flag << 8;

#if 0
   result.num_slice_groups_minus1 = pic->pps->num_slice_groups_minus1;
   result.slice_group_map_type = pic->pps->slice_group_map_type;
   result.slice_group_change_rate_minus1 = pic->pps->slice_group_change_rate_minus1;
#endif

   result.pic_init_qp_minus26 = pps->pic_init_qp_minus26;
   result.chroma_qp_index_offset = pps->chroma_qp_index_offset;
   result.second_chroma_qp_index_offset = pps->second_chroma_qp_index_offset;

   if (pps->flags.scaling_matrix_present_flag) {
      memcpy(result.scaling_list_4x4, pps->pScalingLists->ScalingList4x4, 6 * 16);
      memcpy(result.scaling_list_8x8, pps->pScalingLists->ScalingList8x8, 2 * 64);
   } else {
      memset(result.scaling_list_4x4, 0x10, 6*16);
      memset(result.scaling_list_8x8, 0x10, 2*64);
   }

   memcpy(it_ptr, result.scaling_list_4x4, 6 * 16);
   memcpy((char *)it_ptr + 96, result.scaling_list_8x8, 2 * 64);

   result.num_ref_idx_l0_active_minus1 = pps->num_ref_idx_l0_default_active_minus1;
   result.num_ref_idx_l1_active_minus1 = pps->num_ref_idx_l1_default_active_minus1;

   result.curr_field_order_cnt_list[0] = h264_pic_info->pStdPictureInfo->PicOrderCnt[0];
   result.curr_field_order_cnt_list[1] = h264_pic_info->pStdPictureInfo->PicOrderCnt[1];

   result.frame_num = h264_pic_info->pStdPictureInfo->frame_num;

   result.num_ref_frames = frame_info->referenceSlotCount;
   for (unsigned i = 0; i < frame_info->referenceSlotCount; i++) {
      int idx = frame_info->pReferenceSlots[i].slotIndex;
      const struct VkVideoDecodeH264DpbSlotInfoEXT *dpb_slot =
         vk_find_struct_const(frame_info->pReferenceSlots[i].pNext, VIDEO_DECODE_H264_DPB_SLOT_INFO_EXT);

      result.frame_num_list[idx] = dpb_slot->pStdReferenceInfo->FrameNum;
      result.field_order_cnt_list[idx][0] = dpb_slot->pStdReferenceInfo->PicOrderCnt[0];
      result.field_order_cnt_list[idx][1] = dpb_slot->pStdReferenceInfo->PicOrderCnt[1];
   }
   result.decoded_pic_idx = frame_info->pSetupReferenceSlot->slotIndex;

   return result;
}

static rvcn_dec_message_hevc_t get_h265_msg(struct radv_video_session *vid,
                                            struct radv_video_session_params *params,
                                            const struct VkVideoDecodeInfoKHR *frame_info,
                                            void *it_ptr)
{
   rvcn_dec_message_hevc_t result;
   int i;
   const struct VkVideoDecodeH265PictureInfoEXT *h265_pic_info =
      vk_find_struct_const(frame_info->pNext, VIDEO_DECODE_H265_PICTURE_INFO_EXT);
   memset(&result, 0, sizeof(result));

   const StdVideoH265SequenceParameterSet *sps = &params->h265_dec.sps_std[h265_pic_info->pStdPictureInfo->sps_seq_parameter_set_id];
   const StdVideoH265PictureParameterSet *pps = &params->h265_dec.pps_std[h265_pic_info->pStdPictureInfo->pps_pic_parameter_set_id];

   result.sps_info_flags = 0;
   result.sps_info_flags |= sps->flags.scaling_list_enabled_flag << 0;
   result.sps_info_flags |= sps->flags.amp_enabled_flag << 1;
   result.sps_info_flags |= sps->flags.sample_adaptive_offset_enabled_flag << 2;
   result.sps_info_flags |= sps->flags.pcm_enabled_flag << 3;
   result.sps_info_flags |= sps->flags.pcm_loop_filter_disabled_flag << 4;
   result.sps_info_flags |= sps->flags.long_term_ref_pics_present_flag << 5;
   result.sps_info_flags |= sps->flags.sps_temporal_mvp_enabled_flag << 6;
   result.sps_info_flags |= sps->flags.strong_intra_smoothing_enabled_flag << 7;
   result.sps_info_flags |= sps->flags.separate_colour_plane_flag << 8;

   result.chroma_format = sps->chroma_format_idc;
   result.bit_depth_luma_minus8 = sps->bit_depth_luma_minus8;
   result.bit_depth_chroma_minus8 = sps->bit_depth_chroma_minus8;
   result.log2_max_pic_order_cnt_lsb_minus4 = sps->log2_max_pic_order_cnt_lsb_minus4;
   result.sps_max_dec_pic_buffering_minus1 = sps->sps_max_dec_pic_buffering_minus1;
   result.log2_min_luma_coding_block_size_minus3 =
      sps->log2_min_luma_coding_block_size_minus3;
   result.log2_diff_max_min_luma_coding_block_size =
      sps->log2_diff_max_min_luma_coding_block_size;
   result.log2_min_transform_block_size_minus2 =
      sps->log2_min_luma_transform_block_size_minus2;
   result.log2_diff_max_min_transform_block_size =
      sps->log2_diff_max_min_luma_transform_block_size;
   result.max_transform_hierarchy_depth_inter = sps->max_transform_hierarchy_depth_inter;
   result.max_transform_hierarchy_depth_intra = sps->max_transform_hierarchy_depth_intra;
   result.pcm_sample_bit_depth_luma_minus1 = sps->pcm_sample_bit_depth_luma_minus1;
   result.pcm_sample_bit_depth_chroma_minus1 = sps->pcm_sample_bit_depth_chroma_minus1;
   result.log2_min_pcm_luma_coding_block_size_minus3 =
      sps->log2_min_pcm_luma_coding_block_size_minus3;
   result.log2_diff_max_min_pcm_luma_coding_block_size =
      sps->log2_diff_max_min_pcm_luma_coding_block_size;
   result.num_short_term_ref_pic_sets = sps->num_short_term_ref_pic_sets;

   result.pps_info_flags = 0;
   result.pps_info_flags |= pps->flags.dependent_slice_segments_enabled_flag << 0;
   result.pps_info_flags |= pps->flags.output_flag_present_flag << 1;
   result.pps_info_flags |= pps->flags.sign_data_hiding_enabled_flag << 2;
   result.pps_info_flags |= pps->flags.cabac_init_present_flag << 3;
   result.pps_info_flags |= pps->flags.constrained_intra_pred_flag << 4;
   result.pps_info_flags |= pps->flags.transform_skip_enabled_flag << 5;
   result.pps_info_flags |= pps->flags.cu_qp_delta_enabled_flag << 6;
   result.pps_info_flags |= pps->flags.pps_slice_chroma_qp_offsets_present_flag << 7;
   result.pps_info_flags |= pps->flags.weighted_pred_flag << 8;
   result.pps_info_flags |= pps->flags.weighted_bipred_flag << 9;
   result.pps_info_flags |= pps->flags.transquant_bypass_enabled_flag << 10;
   result.pps_info_flags |= pps->flags.tiles_enabled_flag << 11;
   result.pps_info_flags |= pps->flags.entropy_coding_sync_enabled_flag << 12;
   result.pps_info_flags |= pps->flags.uniform_spacing_flag << 13;
   result.pps_info_flags |= pps->flags.loop_filter_across_tiles_enabled_flag << 14;
   result.pps_info_flags |= pps->flags.pps_loop_filter_across_slices_enabled_flag << 15;
   result.pps_info_flags |= pps->flags.deblocking_filter_override_enabled_flag << 16;
   result.pps_info_flags |= pps->flags.pps_deblocking_filter_disabled_flag << 17;
   result.pps_info_flags |= pps->flags.lists_modification_present_flag << 18;
   result.pps_info_flags |= pps->flags.slice_segment_header_extension_present_flag << 19;

   result.num_extra_slice_header_bits = pps->num_extra_slice_header_bits;
   result.num_long_term_ref_pic_sps = sps->num_long_term_ref_pics_sps;
   result.num_ref_idx_l0_default_active_minus1 = pps->num_ref_idx_l0_default_active_minus1;
   result.num_ref_idx_l1_default_active_minus1 = pps->num_ref_idx_l1_default_active_minus1;
   result.pps_cb_qp_offset = pps->pps_cb_qp_offset;
   result.pps_cr_qp_offset = pps->pps_cr_qp_offset;
   result.pps_beta_offset_div2 = pps->pps_beta_offset_div2;
   result.pps_tc_offset_div2 = pps->pps_tc_offset_div2;
   result.diff_cu_qp_delta_depth = pps->diff_cu_qp_delta_depth;
   result.num_tile_columns_minus1 = pps->num_tile_columns_minus1;
   result.num_tile_rows_minus1 = pps->num_tile_rows_minus1;
   result.log2_parallel_merge_level_minus2 = pps->log2_parallel_merge_level_minus2;
   result.init_qp_minus26 = pps->init_qp_minus26;

   for (i = 0; i < 19; ++i)
      result.column_width_minus1[i] = pps->column_width_minus1[i];

   for (i = 0; i < 21; ++i)
      result.row_height_minus1[i] = pps->row_height_minus1[i];

   result.num_delta_pocs_ref_rps_idx = h265_pic_info->pStdPictureInfo->NumDeltaPocsOfRefRpsIdx;
   result.curr_poc = h265_pic_info->pStdPictureInfo->PicOrderCntVal;

   for (i = 0; i < frame_info->referenceSlotCount; i++) {
      const struct VkVideoDecodeH265DpbSlotInfoEXT *dpb_slot =
         vk_find_struct_const(frame_info->pReferenceSlots[i].pNext, VIDEO_DECODE_H265_DPB_SLOT_INFO_EXT);

      result.poc_list[frame_info->pReferenceSlots[i].slotIndex] = dpb_slot->pStdReferenceInfo->PicOrderCntVal;
      result.ref_pic_list[i] = frame_info->pReferenceSlots[i].slotIndex;
   }

   for (i = 0; i < 8; ++i)
      result.ref_pic_set_st_curr_before[i] = h265_pic_info->pStdPictureInfo->RefPicSetStCurrBefore[i];

   for (i = 0; i < 8; ++i)
      result.ref_pic_set_st_curr_after[i] = h265_pic_info->pStdPictureInfo->RefPicSetStCurrAfter[i];

   for (i = 0; i < 8; ++i)
      result.ref_pic_set_lt_curr[i] = h265_pic_info->pStdPictureInfo->RefPicSetLtCurr[i];

   if (sps->flags.sps_scaling_list_data_present_flag) {
      for (i = 0; i < 6; ++i)
         result.ucScalingListDCCoefSizeID2[i] = sps->pScalingLists->ScalingListDCCoef16x16[i];

      for (i = 0; i < 2; ++i)
         result.ucScalingListDCCoefSizeID3[i] = sps->pScalingLists->ScalingListDCCoef32x32[i];

      memcpy(it_ptr, sps->pScalingLists->ScalingList4x4, 6 * 16);
      memcpy((char *)it_ptr + 96, sps->pScalingLists->ScalingList8x8, 6 * 64);
      memcpy((char *)it_ptr + 480, sps->pScalingLists->ScalingList16x16, 6 * 64);
      memcpy((char *)it_ptr + 864, sps->pScalingLists->ScalingList32x32, 2 * 64);
   }

#if 0
   for (i = 0; i < 2; i++) {
      for (j = 0; j < 15; j++)
         result.direct_reflist[i][j] = pic->RefPicList[i][j];
   }

   if (pic->base.profile == PIPE_VIDEO_PROFILE_HEVC_MAIN_10) {
      if (target->buffer_format == PIPE_FORMAT_P010 || target->buffer_format == PIPE_FORMAT_P016) {
         result.p010_mode = 1;
         result.msb_mode = 1;
      } else {
         result.p010_mode = 0;
         result.luma_10to8 = 5;
         result.chroma_10to8 = 5;
         result.hevc_reserved[0] = 4; /* sclr_luma10to8 */
         result.hevc_reserved[1] = 4; /* sclr_chroma10to8 */
      }
   }
#endif
   return result;
}

static bool rvcn_dec_message_decode(struct radv_video_session *vid,
                                    struct radv_video_session_params *params,
                                    void *ptr,
                                    void *it_ptr,
                                    const struct VkVideoDecodeInfoKHR *frame_info)
{
   rvcn_dec_message_header_t *header;
   rvcn_dec_message_index_t *index_codec;
   rvcn_dec_message_decode_t *decode;
   void *codec;
   unsigned sizes = 0, offset_decode, offset_codec;
   struct radv_image_view *dst_iv = radv_image_view_from_handle(frame_info->dstPictureResource.imageViewBinding);
   struct radv_image *img = dst_iv->image;

   struct radv_image_plane *luma = &img->planes[0];
   struct radv_image_plane *chroma = &img->planes[1];

   header = ptr;
   sizes += sizeof(rvcn_dec_message_header_t);

   index_codec = (void *)((char *)header + sizes);
   sizes += sizeof(rvcn_dec_message_index_t);

   //encrypted
   //dpb > TIER_1

   offset_decode = sizes;
   decode = (void *)((char*)header + sizes);
   sizes += sizeof(rvcn_dec_message_decode_t);

   //encrypted
   //tier1

   offset_codec = sizes;
   codec = (void *)((char *)header + sizes);

   memset(ptr, 0, sizes);

   header->header_size = sizeof(rvcn_dec_message_header_t);
   header->total_size = sizes;
   header->msg_type = RDECODE_MSG_DECODE;
   header->stream_handle = vid->stream_handle;
   header->status_report_feedback_number = vid->dbg_frame_cnt++;

   header->index[0].message_id = RDECODE_MESSAGE_DECODE;
   header->index[0].offset = offset_decode;
   header->index[0].size = sizeof(rvcn_dec_message_decode_t);
   header->index[0].filled = 0;
   header->num_buffers = 1;

   index_codec->offset = offset_codec;
   index_codec->size = sizeof(rvcn_dec_message_avc_t);
   index_codec->filled = 0;
   ++header->num_buffers;

   //encrypted
   // dpb_type

   decode->stream_type = vid->stream_type;
   decode->decode_flags = 0;
   decode->width_in_samples = frame_info->codedExtent.width;
   decode->height_in_samples = frame_info->codedExtent.height;

   decode->bsd_size = frame_info->srcBufferRange;

   decode->dpb_size = (vid->dpb_type != DPB_DYNAMIC_TIER_2) ? vid->dpb.size : 0;

   decode->dt_size = dst_iv->image->planes[0].surface.total_size +
      dst_iv->image->planes[1].surface.total_size;
   decode->sct_size = 0;
   decode->sc_coeff_size = 0;

   decode->sw_ctxt_size = RDECODE_SESSION_CONTEXT_SIZE;
   decode->db_pitch = align(frame_info->codedExtent.width, vid->db_alignment);

   decode->db_surf_tile_config = 0;

   decode->dt_pitch = luma->surface.u.gfx9.surf_pitch * luma->surface.blk_w;
   decode->dt_uv_pitch = chroma->surface.u.gfx9.surf_pitch * chroma->surface.blk_w;

   if (luma->surface.meta_offset) {
      fprintf(stderr, "DCC SURFACES NOT SUPPORTED.\n");
      return false;
   }

   decode->dt_tiling_mode = 0;
   decode->dt_swizzle_mode = luma->surface.u.gfx9.swizzle_mode;
   decode->dt_array_mode = RDECODE_ARRAY_MODE_LINEAR;
   decode->dt_field_mode = vid->interlaced ? 1 : 0;
   decode->dt_surf_tile_config = 0;
   decode->dt_uv_surf_tile_config = 0;

   decode->dt_luma_top_offset = luma->surface.u.gfx9.surf_offset;
   decode->dt_chroma_top_offset = chroma->surface.u.gfx9.surf_offset;

   if (decode->dt_field_mode) {
      decode->dt_luma_bottom_offset =
         luma->surface.u.gfx9.surf_offset + luma->surface.u.gfx9.surf_slice_size;
      decode->dt_chroma_bottom_offset =
         chroma->surface.u.gfx9.surf_offset + chroma->surface.u.gfx9.surf_slice_size;
   } else {
      decode->dt_luma_bottom_offset = decode->dt_luma_top_offset;
      decode->dt_chroma_bottom_offset = decode->dt_chroma_top_offset;
   }

   switch (vid->op) {
   case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_EXT: {
      rvcn_dec_message_avc_t avc = get_h264_msg(vid, params, frame_info, it_ptr);
      memcpy(codec, (void *)&avc, sizeof(rvcn_dec_message_avc_t));
      index_codec->message_id = RDECODE_MESSAGE_AVC;
      break;
   }
   case VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_EXT: {
      rvcn_dec_message_hevc_t hevc = get_h265_msg(vid, params, frame_info, it_ptr);
      memcpy(codec, (void *)&hevc, sizeof(rvcn_dec_message_hevc_t));
      index_codec->message_id = RDECODE_MESSAGE_HEVC;
      break;
   }
   default:
      break;
   }

   decode->hw_ctxt_size = vid->ctx.size;

   return true;
}

void
radv_CmdBeginVideoCodingKHR(VkCommandBuffer commandBuffer,
                            const VkVideoBeginCodingInfoKHR *pBeginInfo)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   RADV_FROM_HANDLE(radv_video_session, vid, pBeginInfo->videoSession);
   RADV_FROM_HANDLE(radv_video_session_params, params, pBeginInfo->videoSessionParameters);

   send_cmd(cmd_buffer->device, cmd_buffer->cs, RDECODE_CMD_SESSION_CONTEXT_BUFFER, vid->sessionctx.mem->bo, vid->sessionctx.offset);
#if !defined(SEND_ON_CREATE_HACK)
   uint32_t size = sizeof(rvcn_dec_message_header_t) + sizeof(rvcn_dec_message_create_t);

   void *ptr;
   uint32_t out_offset;

   radv_cmd_buffer_upload_alloc(cmd_buffer, size, &out_offset,
                                &ptr);

   rvcn_dec_message_create(vid, ptr, size);

   send_cmd(cmd_buffer->device, cmd_buffer->cs, RDECODE_CMD_MSG_BUFFER, cmd_buffer->upload.upload_bo, out_offset);
#endif
   cmd_buffer->video.vid = vid;
   cmd_buffer->video.params = params;
}

void
radv_CmdControlVideoCodingKHR(VkCommandBuffer commandBuffer,
                              const VkVideoCodingControlInfoKHR *pCodingControlInfo)
{


}

void
radv_CmdEndVideoCodingKHR(VkCommandBuffer commandBuffer,
                          const VkVideoEndCodingInfoKHR *pEndCodingInfo)
{
#if !defined(SEND_ON_CREATE_HACK)
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_video_session *vid = cmd_buffer->video.vid;
   uint32_t size = sizeof(rvcn_dec_message_header_t);
   void *ptr;
   uint32_t out_offset;
   radv_cmd_buffer_upload_alloc(cmd_buffer, size, &out_offset,
                                &ptr);
   rvcn_dec_message_destroy(vid->stream_handle, ptr, size);
   send_cmd(cmd_buffer->device, cmd_buffer->cs, RDECODE_CMD_SESSION_CONTEXT_BUFFER, vid->sessionctx.mem->bo, vid->sessionctx.offset);
   send_cmd(cmd_buffer->device, cmd_buffer->cs, RDECODE_CMD_MSG_BUFFER, cmd_buffer->upload.upload_bo, out_offset);
#endif
}

void
radv_CmdDecodeVideoKHR(VkCommandBuffer commandBuffer,
                       const VkVideoDecodeInfoKHR *frame_info)
{
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   RADV_FROM_HANDLE(radv_buffer, src_buffer, frame_info->srcBuffer);
   struct radv_video_session *vid = cmd_buffer->video.vid;
   struct radv_video_session_params *params = cmd_buffer->video.params;
   unsigned size = 0;
   void *ptr, *fb_ptr, *it_ptr = NULL;
   uint32_t out_offset, fb_offset, it_offset = 0;
   struct radeon_winsys_bo *msg_bo, *fb_bo, *it_bo = NULL;

   size += sizeof(rvcn_dec_message_header_t);
   size += sizeof(rvcn_dec_message_index_t);
   //encrypted
   //dpb
   size += sizeof(rvcn_dec_message_decode_t);
   switch (vid->op) {
   case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_EXT:
      size += sizeof(rvcn_dec_message_avc_t);
      break;
   case VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_EXT:
      size += sizeof(rvcn_dec_message_hevc_t);
      break;
   }

   //encrypted

   radv_cmd_buffer_upload_alloc(cmd_buffer, FB_BUFFER_SIZE, &fb_offset,
                                &fb_ptr);
   fb_bo = cmd_buffer->upload.upload_bo;
   if (have_it(vid)) {
      radv_cmd_buffer_upload_alloc(cmd_buffer, IT_SCALING_TABLE_SIZE, &it_offset,
                                   &it_ptr);
      it_bo = cmd_buffer->upload.upload_bo;
   }

   radv_cmd_buffer_upload_alloc(cmd_buffer, size, &out_offset,
                                &ptr);
   msg_bo = cmd_buffer->upload.upload_bo;

   rvcn_dec_message_decode(vid, params, ptr, it_ptr, frame_info);
   rvcn_dec_message_feedback(fb_ptr);
   send_cmd(cmd_buffer->device, cmd_buffer->cs, RDECODE_CMD_SESSION_CONTEXT_BUFFER, vid->sessionctx.mem->bo, vid->sessionctx.offset);
   send_cmd(cmd_buffer->device, cmd_buffer->cs, RDECODE_CMD_MSG_BUFFER, msg_bo, out_offset);
   if (vid->dpb.mem && vid->dpb_type != DPB_DYNAMIC_TIER_2)
      send_cmd(cmd_buffer->device, cmd_buffer->cs, RDECODE_CMD_DPB_BUFFER, vid->dpb.mem->bo, vid->dpb.offset);

   if (vid->ctx.mem)
      send_cmd(cmd_buffer->device, cmd_buffer->cs, RDECODE_CMD_CONTEXT_BUFFER, vid->ctx.mem->bo, vid->ctx.offset);

   send_cmd(cmd_buffer->device, cmd_buffer->cs, RDECODE_CMD_BITSTREAM_BUFFER, src_buffer->bo, src_buffer->offset + frame_info->srcBufferOffset);

   struct radv_image_view *dst_iv = radv_image_view_from_handle(frame_info->dstPictureResource.imageViewBinding);
   struct radv_image *img = dst_iv->image;
   send_cmd(cmd_buffer->device, cmd_buffer->cs, RDECODE_CMD_DECODING_TARGET_BUFFER, img->bo, img->offset);
   send_cmd(cmd_buffer->device, cmd_buffer->cs, RDECODE_CMD_FEEDBACK_BUFFER, fb_bo, fb_offset);
   if (have_it(vid))
      send_cmd(cmd_buffer->device, cmd_buffer->cs, RDECODE_CMD_IT_SCALING_TABLE_BUFFER, it_bo, it_offset);
   else if (have_probs(vid))
      send_cmd(cmd_buffer->device, cmd_buffer->cs, RDECODE_CMD_PROB_TBL_BUFFER, NULL, 0);

   set_reg(cmd_buffer->cs, cmd_buffer->device->physical_device->vid_dec_reg.cntl, 1);
}

void
radv_CmdEncodeVideoKHR(VkCommandBuffer commandBuffer,
                       const VkVideoEncodeInfoKHR *pEncodeInfo)
{
}
