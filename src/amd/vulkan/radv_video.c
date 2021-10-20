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
   return false;
   //   return dec->stream_type == RDECODE_CODEC_H264_PERF || dec->stream_type == RDECODE_CODEC_H265;
}

/* do the codec needs an probs buffer? */
static bool have_probs(struct radv_video_session *vid)
{
   return false;
   //   return (dec->stream_type == RDECODE_CODEC_VP9 || dec->stream_type == RDECODE_CODEC_AV1);
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


   //   case PIPE_VIDEO_FORMAT_MPEG4_AVC: {
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
#if 0
      break;
   }

   case PIPE_VIDEO_FORMAT_HEVC:
      if (dec->base.width * dec->base.height >= 4096 * 2000)
         max_references = MAX2(max_references, 8);
      else
         max_references = MAX2(max_references, 17);

      width = align(width, 16);
      height = align(height, 16);
      if (dec->base.profile == PIPE_VIDEO_PROFILE_HEVC_MAIN_10)
         dpb_size = align((align(width, 64) * align(height, 64) * 9) / 4, 256) * max_references;
      else
         dpb_size = align((align(width, 32) * height * 3) / 2, 256) * max_references;
      break;

   case PIPE_VIDEO_FORMAT_VC1:
      // the firmware seems to allways assume a minimum of ref frames
      max_references = MAX2(NUM_VC1_REFS, max_references);

      // reference picture buffer
      dpb_size = image_size * max_references;

      // CONTEXT_BUFFER
      dpb_size += width_in_mb * height_in_mb * 128;

      // IT surface buffer
      dpb_size += width_in_mb * 64;

      // DB surface buffer
      dpb_size += width_in_mb * 128;

      // BP
      dpb_size += align(MAX2(width_in_mb, height_in_mb) * 7 * 16, 64);
      break;

   case PIPE_VIDEO_FORMAT_MPEG12:
      // reference picture buffer, must be big enough for all frames
      dpb_size = image_size * NUM_MPEG2_REFS;
      break;

   case PIPE_VIDEO_FORMAT_MPEG4:
      // reference picture buffer
      dpb_size = image_size * max_references;

      // CM
      dpb_size += width_in_mb * height_in_mb * 64;

      // IT surface buffer
      dpb_size += align(width_in_mb * height_in_mb * 32, 64);

      dpb_size = MAX2(dpb_size, 30 * 1024 * 1024);
      break;

   case PIPE_VIDEO_FORMAT_VP9:
      max_references = MAX2(max_references, 9);

      if (dec->dpb_type == DPB_MAX_RES)
         dpb_size = (((struct si_screen *)dec->screen)->info.family >= CHIP_RENOIR)
            ? (8192 * 4320 * 3 / 2) * max_references
            : (4096 * 3000 * 3 / 2) * max_references;
      else
         dpb_size = (align(dec->base.width, dec->db_alignment) *
            align(dec->base.height, dec->db_alignment) * 3 / 2) * max_references;

      if (dec->base.profile == PIPE_VIDEO_PROFILE_VP9_PROFILE2)
         dpb_size = dpb_size * 3 / 2;
      break;

   case PIPE_VIDEO_FORMAT_AV1:
      max_references = MAX2(max_references, 9);
      dpb_size = 8192 * 4320 * 3 / 2 * max_references * 3 / 2;
      break;

   case PIPE_VIDEO_FORMAT_JPEG:
      dpb_size = 0;
      break;

   default:
      // something is missing here
      assert(0);

      // at least use a sane default value
      dpb_size = 32 * 1024 * 1024;
      break;
   }
#endif
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

   vid->stream_handle = si_vid_alloc_stream_handle();
   uint32_t max_ref = vid->max_ref_pic_slots;
   vid->level = u_get_h264_level(pCreateInfo->maxCodedExtent.width,
				 pCreateInfo->maxCodedExtent.height, &max_ref);

#if 0
   if (device->physical_device->rad_info.family >= CHIP_SIENNA_CICHLID &&
       (stream_type == RDECODE_CODEC_VP9 || stream_type == RDECODE_CODEC_AV1))
      vid->dpb_type = DPB_DYNAMIC_TIER_2;
   else if (device->physical_device->rad_info.family <= CHIP_NAVI14 && stream_type == RDECODE_CODEC_VP9)
      vid->dpb_type = DPB_DYNAMIC_TIER_1;
   else
#endif
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

   struct radv_video_session_params *params =
      vk_alloc2(&device->vk.alloc, pAllocator, sizeof(*params), 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!params)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&device->vk, &params->base, VK_OBJECT_TYPE_VIDEO_SESSION_PARAMETERS_KHR);

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

   vk_object_base_finish(&params->base);
   vk_free2(&device->vk.alloc, pAllocator, params);
}

VkResult
radv_GetPhysicalDeviceVideoCapabilitiesKHR(VkPhysicalDevice physicalDevice,
                                           const VkVideoProfileKHR *pVideoProfile,
                                           VkVideoCapabilitiesKHR *pCapabilities)
{
   pCapabilities->capabilityFlags = 0;
   pCapabilities->minBitstreamBufferOffsetAlignment = 0;
   pCapabilities->minBitstreamBufferSizeAlignment = 0;
   pCapabilities->videoPictureExtentGranularity.width = 1;
   pCapabilities->videoPictureExtentGranularity.height = 1;
   pCapabilities->minExtent.width = 1;
   pCapabilities->minExtent.height = 1;
   // TODO
   return VK_SUCCESS;
}

VkResult
radv_GetPhysicalDeviceVideoFormatPropertiesKHR(VkPhysicalDevice physicalDevice,
                                               const VkPhysicalDeviceVideoFormatInfoKHR *pVideoFormatInfo,
                                               uint32_t *pVideoFormatPropertyCount,
                                               VkVideoFormatPropertiesKHR *pVideoFormatProperties)
{
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
   *pVideoSessionMemoryRequirementsCount = 5;

   if (!pVideoSessionMemoryRequirements)
      return VK_SUCCESS;

   /* 1 buffer for session context */
   pVideoSessionMemoryRequirements[0].pMemoryRequirements->memoryRequirements.size = RDECODE_SESSION_CONTEXT_SIZE;
   pVideoSessionMemoryRequirements[0].pMemoryRequirements->memoryRequirements.alignment = 0;
   pVideoSessionMemoryRequirements[0].pMemoryRequirements->memoryRequirements.memoryTypeBits = memory_type_bits;
   /* 4 buffers for msg_fb_it_probs */

   unsigned msg_fb_it_probs_size = FB_BUFFER_OFFSET + FB_BUFFER_SIZE;
   if (have_it(vid))
      msg_fb_it_probs_size += IT_SCALING_TABLE_SIZE;
   else if (have_probs(vid))
      msg_fb_it_probs_size += 0;//TODO

   for (unsigned i = 1; i < 5; i++) {
      pVideoSessionMemoryRequirements[i].pMemoryRequirements->memoryRequirements.size = msg_fb_it_probs_size;
      pVideoSessionMemoryRequirements[i].pMemoryRequirements->memoryRequirements.alignment = 0;
      pVideoSessionMemoryRequirements[i].pMemoryRequirements->memoryRequirements.memoryTypeBits = memory_type_bits;
   }

   return VK_SUCCESS;
}

VkResult
radv_UpdateVideoSessionParametersKHR(VkDevice _device,
                                     VkVideoSessionParametersKHR videoSessionParameters,
                                     const VkVideoSessionParametersUpdateInfoKHR *pUpdateInfo)
{
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

   assert(videoSessionBindMemoryCount == 5);
   copy_bind(&vid->ctx, &pVideoSessionBindMemories[0]);
   for (unsigned i = 0; i < 4; i++)
      copy_bind(&vid->fb_it[i], &pVideoSessionBindMemories[i + 1]);
   return VK_SUCCESS;
}

void
radv_CmdBeginVideoCodingKHR(VkCommandBuffer commandBuffer,
                            const VkVideoBeginCodingInfoKHR *pBeginInfo)
{

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

}

void radv_CmdDecodeVideoKHR(VkCommandBuffer commandBuffer,
                            const VkVideoDecodeInfoKHR *pFrameInfo)
{

}

void radv_CmdEncodeVideoKHR(VkCommandBuffer commandBuffer,
                            const VkVideoEncodeInfoKHR *pEncodeInfo)
{
}
