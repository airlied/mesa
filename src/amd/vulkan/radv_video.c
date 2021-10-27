#include "radv_private.h"

#include "ac_vcn_dec_regs.h"

#define NUM_H264_REFS  17
#define FB_BUFFER_OFFSET             0x1000
#define FB_BUFFER_SIZE               2048
#define IT_SCALING_TABLE_SIZE        992
#define VP9_PROBS_TABLE_SIZE         (RDECODE_VP9_PROBS_DATA_SIZE + 256)
#define RDECODE_SESSION_CONTEXT_SIZE (128 * 1024)

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

      break;
   default:
      return VK_ERROR_FEATURE_NOT_PRESENT;
   }

   vid->stream_handle = si_vid_alloc_stream_handle();
   vid->stream_type = RDECODE_CODEC_H264_PERF;
   vid->dbg_frame_cnt = 0;
   uint32_t max_ref = vid->max_ref_pic_slots;
   vid->level = u_get_h264_level(pCreateInfo->maxCodedExtent.width,
                                 pCreateInfo->maxCodedExtent.height, &max_ref);

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

   if (vid->stream_type == RDECODE_CODEC_H264_PERF)
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
      pVideoSessionMemoryRequirements[idx].pMemoryRequirements->memoryRequirements.size = calc_ctx_size_h264_perf(vid);
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
static void set_reg(struct radv_cmd_buffer *cmd_buffer, unsigned reg, uint32_t val)
{
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   radeon_emit(cs, RDECODE_PKT0(reg >> 2, 0));
   radeon_emit(cs, val);
}

static void send_cmd(struct radv_cmd_buffer *cmd_buffer, unsigned cmd,
                     struct radeon_winsys_bo *bo, uint32_t offset)
{
   struct radv_physical_device *pdev = cmd_buffer->device->physical_device;
   uint64_t addr;

   radv_cs_add_buffer(cmd_buffer->device->ws, cmd_buffer->cs, bo);
   addr = radv_buffer_get_va(bo);
   addr += offset;
   set_reg(cmd_buffer, pdev->vid_dec_reg.data0, addr);
   set_reg(cmd_buffer, pdev->vid_dec_reg.data1, addr >> 32);
   set_reg(cmd_buffer, pdev->vid_dec_reg.cmd, cmd << 1);
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

   // MPEG4_AVC only.

   rvcn_dec_message_avc_t avc = get_h264_msg(vid, params, frame_info, it_ptr);
   memcpy(codec, (void *)&avc, sizeof(rvcn_dec_message_avc_t));
   index_codec->message_id = RDECODE_MESSAGE_AVC;

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

   uint32_t size = sizeof(rvcn_dec_message_header_t) + sizeof(rvcn_dec_message_create_t);

   void *ptr;
   uint32_t out_offset;

   radv_cmd_buffer_upload_alloc(cmd_buffer, size, &out_offset,
                                &ptr);

   rvcn_dec_message_create(vid, ptr, size);

   send_cmd(cmd_buffer, RDECODE_CMD_SESSION_CONTEXT_BUFFER, vid->sessionctx.mem->bo, vid->sessionctx.offset);
   send_cmd(cmd_buffer, RDECODE_CMD_MSG_BUFFER, cmd_buffer->upload.upload_bo, out_offset);

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
   RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_video_session *vid = cmd_buffer->video.vid;
   uint32_t size = sizeof(rvcn_dec_message_header_t);
   void *ptr;
   uint32_t out_offset;
   radv_cmd_buffer_upload_alloc(cmd_buffer, size, &out_offset,
                                &ptr);
   rvcn_dec_message_destroy(vid->stream_handle, ptr, size);
   send_cmd(cmd_buffer, RDECODE_CMD_SESSION_CONTEXT_BUFFER, vid->sessionctx.mem->bo, vid->sessionctx.offset);
   send_cmd(cmd_buffer, RDECODE_CMD_MSG_BUFFER, cmd_buffer->upload.upload_bo, out_offset);
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
   size += sizeof(rvcn_dec_message_avc_t);
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
   send_cmd(cmd_buffer, RDECODE_CMD_SESSION_CONTEXT_BUFFER, vid->sessionctx.mem->bo, vid->sessionctx.offset);
   send_cmd(cmd_buffer, RDECODE_CMD_MSG_BUFFER, msg_bo, out_offset);

   if (vid->dpb.mem && vid->dpb_type != DPB_DYNAMIC_TIER_2)
      send_cmd(cmd_buffer, RDECODE_CMD_DPB_BUFFER, vid->dpb.mem->bo, vid->dpb.offset);

   if (vid->ctx.mem)
      send_cmd(cmd_buffer, RDECODE_CMD_CONTEXT_BUFFER, vid->ctx.mem->bo, vid->ctx.offset);

   send_cmd(cmd_buffer, RDECODE_CMD_BITSTREAM_BUFFER, src_buffer->bo, src_buffer->offset + frame_info->srcBufferOffset);

   struct radv_image_view *dst_iv = radv_image_view_from_handle(frame_info->dstPictureResource.imageViewBinding);
   struct radv_image *img = dst_iv->image;
   send_cmd(cmd_buffer, RDECODE_CMD_DECODING_TARGET_BUFFER, img->bo, img->offset);
   send_cmd(cmd_buffer, RDECODE_CMD_FEEDBACK_BUFFER, fb_bo, fb_offset);
   if (have_it(vid))
      send_cmd(cmd_buffer, RDECODE_CMD_IT_SCALING_TABLE_BUFFER, it_bo, it_offset);
   else if (have_probs(vid))
      send_cmd(cmd_buffer, RDECODE_CMD_PROB_TBL_BUFFER, NULL, 0);

   set_reg(cmd_buffer, cmd_buffer->device->physical_device->vid_dec_reg.cntl, 1);
}

void
radv_CmdEncodeVideoKHR(VkCommandBuffer commandBuffer,
                       const VkVideoEncodeInfoKHR *pEncodeInfo)
{
}
