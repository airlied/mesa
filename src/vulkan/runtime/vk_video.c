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

#include "vk_video.h"
#include "vk_util.h"
#include "vk_alloc.h"
#include "vk_device.h"
#include "vl_rbsp.h"

#ifdef VK_ENABLE_BETA_EXTENSIONS

VkResult
vk_video_session_init(struct vk_device *device,
                      struct vk_video_session *vid,
                      const VkVideoSessionCreateInfoKHR *create_info)
{
   vk_object_base_init(device, &vid->base, VK_OBJECT_TYPE_VIDEO_SESSION_KHR);

   vid->op = create_info->pVideoProfile->videoCodecOperation;
   vid->max_coded = create_info->maxCodedExtent;
   vid->picture_format = create_info->pictureFormat;
   vid->ref_format = create_info->referencePicturesFormat;
   vid->max_ref_pic_slots = create_info->maxReferencePicturesSlotsCount;
   vid->max_ref_pic_active = create_info->maxReferencePicturesActiveCount;

   switch (vid->op) {
   case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_EXT: {
      const struct VkVideoDecodeH264ProfileEXT *h264_profile =
         vk_find_struct_const(create_info->pVideoProfile->pNext, VIDEO_DECODE_H264_PROFILE_EXT);
      vid->h264.profile_idc = h264_profile->stdProfileIdc;
      break;
   }
   case VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_EXT: {
      const struct VkVideoDecodeH265ProfileEXT *h265_profile =
         vk_find_struct_const(create_info->pVideoProfile->pNext, VIDEO_DECODE_H265_PROFILE_EXT);
      vid->h265.profile_idc = h265_profile->stdProfileIdc;
      break;
   }
   default:
      return VK_ERROR_FEATURE_NOT_PRESENT;
   }

   return VK_SUCCESS;
}


static void
init_add_h264_session_parameters(struct vk_video_session_parameters *params,
                                 const struct VkVideoDecodeH264SessionParametersAddInfoEXT *h264_add)
{
   unsigned i, j;
   for (i = 0; i < h264_add->spsStdCount; i++) {
      for (j = 0; j < params->h264_dec.sps_std_count; j++) {
         if (params->h264_dec.sps_std[j].seq_parameter_set_id == h264_add->pSpsStd[i].seq_parameter_set_id) {
            typed_memcpy(&params->h264_dec.sps_std[j], &h264_add->pSpsStd[i], 1);
            break;
         }
      }
      if (j < params->h264_dec.sps_std_count)
         continue;

      typed_memcpy(&params->h264_dec.sps_std[params->h264_dec.sps_std_count], &h264_add->pSpsStd[i], 1);
      params->h264_dec.sps_std_count++;
   }

   for (i = 0; i < h264_add->ppsStdCount; i++) {
      for (j = 0; j < params->h264_dec.pps_std_count; j++) {
         if (params->h264_dec.pps_std[j].pic_parameter_set_id == h264_add->pPpsStd[i].pic_parameter_set_id) {
            typed_memcpy(&params->h264_dec.pps_std[j], &h264_add->pPpsStd[i], 1);
            break;
         }
      }
      if (j < params->h264_dec.pps_std_count)
         continue;

      typed_memcpy(&params->h264_dec.pps_std[params->h264_dec.pps_std_count], &h264_add->pPpsStd[i], 1);
      params->h264_dec.pps_std_count++;
   }
}

static void
init_add_h265_session_parameters(struct vk_video_session_parameters *params,
                                 const struct VkVideoDecodeH265SessionParametersAddInfoEXT *h265_add)
{
   unsigned i, j;
   for (i = 0; i < h265_add->spsStdCount; i++) {
      for (j = 0; j < params->h265_dec.sps_std_count; j++) {
         if (params->h265_dec.sps_std[j].sps_seq_parameter_set_id == h265_add->pSpsStd[i].sps_seq_parameter_set_id) {
            typed_memcpy(&params->h265_dec.sps_std[j], &h265_add->pSpsStd[i], 1);
            break;
         }
      }
      if (j < params->h265_dec.sps_std_count)
         continue;

      typed_memcpy(&params->h265_dec.sps_std[params->h265_dec.sps_std_count], &h265_add->pSpsStd[i], 1);
      params->h265_dec.sps_std_count++;
   }

   for (i = 0; i < h265_add->ppsStdCount; i++) {
      for (j = 0; j < params->h265_dec.pps_std_count; j++) {
         if (params->h265_dec.pps_std[j].pps_pic_parameter_set_id == h265_add->pPpsStd[i].pps_pic_parameter_set_id) {
            typed_memcpy(&params->h265_dec.pps_std[j], &h265_add->pPpsStd[i], 1);
            break;
         }
      }
      if (j < params->h265_dec.pps_std_count)
         continue;

      typed_memcpy(&params->h265_dec.pps_std[params->h265_dec.pps_std_count], &h265_add->pPpsStd[i], 1);
      params->h265_dec.pps_std_count++;
   }
}

VkResult
vk_video_session_parameters_init(struct vk_device *device,
                                 struct vk_video_session_parameters *params,
                                 const struct vk_video_session *vid,
                                 const struct vk_video_session_parameters *templ,
                                 const VkVideoSessionParametersCreateInfoKHR *create_info)
{
   vk_object_base_init(device, &params->base, VK_OBJECT_TYPE_VIDEO_SESSION_PARAMETERS_KHR);

   params->op = vid->op;

   switch (vid->op) {
   case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_EXT: {
      const struct VkVideoDecodeH264SessionParametersCreateInfoEXT *h264_create =
         vk_find_struct_const(create_info->pNext, VIDEO_DECODE_H264_SESSION_PARAMETERS_CREATE_INFO_EXT);

      if (templ) {
         params->h264_dec.max_sps_std_count = templ->h264_dec.max_sps_std_count;
         params->h264_dec.max_pps_std_count = templ->h264_dec.max_pps_std_count;
      } else {
         params->h264_dec.max_sps_std_count = h264_create->maxSpsStdCount;
         params->h264_dec.max_pps_std_count = h264_create->maxPpsStdCount;
      }

      uint32_t sps_size = params->h264_dec.max_sps_std_count * sizeof(StdVideoH264SequenceParameterSet);
      uint32_t pps_size = params->h264_dec.max_pps_std_count * sizeof(StdVideoH264PictureParameterSet);

      params->h264_dec.sps_std = vk_alloc(&device->alloc, sps_size, 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      params->h264_dec.pps_std = vk_alloc(&device->alloc, pps_size, 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      if (!params->h264_dec.sps_std || !params->h264_dec.pps_std) {
         vk_free(&device->alloc, params->h264_dec.sps_std);
         vk_free(&device->alloc, params->h264_dec.pps_std);
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      }

      if (templ) {
         typed_memcpy(params->h264_dec.sps_std, templ->h264_dec.sps_std, templ->h264_dec.sps_std_count);
         params->h264_dec.sps_std_count = templ->h264_dec.sps_std_count;
         typed_memcpy(params->h264_dec.pps_std, templ->h264_dec.pps_std, templ->h264_dec.pps_std_count);
         params->h264_dec.pps_std_count = templ->h264_dec.pps_std_count;
      } else {
         params->h264_dec.sps_std_count = 0;
         params->h264_dec.pps_std_count = 0;
      }

      if (h264_create->pParametersAddInfo)
         init_add_h264_session_parameters(params, h264_create->pParametersAddInfo);
      break;
   }
   case VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_EXT: {
      const struct VkVideoDecodeH265SessionParametersCreateInfoEXT *h265_create =
         vk_find_struct_const(create_info->pNext, VIDEO_DECODE_H265_SESSION_PARAMETERS_CREATE_INFO_EXT);

      if (templ) {
         params->h265_dec.max_sps_std_count = templ->h265_dec.max_sps_std_count;
         params->h265_dec.max_pps_std_count = templ->h265_dec.max_pps_std_count;
      } else {
         params->h265_dec.max_sps_std_count = h265_create->maxSpsStdCount;
         params->h265_dec.max_pps_std_count = h265_create->maxPpsStdCount;
      }

      uint32_t sps_size = params->h265_dec.max_sps_std_count * sizeof(StdVideoH265SequenceParameterSet);
      uint32_t pps_size = params->h265_dec.max_pps_std_count * sizeof(StdVideoH265PictureParameterSet);

      params->h265_dec.sps_std = vk_alloc(&device->alloc, sps_size, 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      params->h265_dec.pps_std = vk_alloc(&device->alloc, pps_size, 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);

      if (!params->h265_dec.sps_std || !params->h265_dec.pps_std) {
         vk_free(&device->alloc, params->h265_dec.sps_std);
         vk_free(&device->alloc, params->h265_dec.pps_std);
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      }

      if (templ) {
         typed_memcpy(params->h265_dec.sps_std, templ->h265_dec.sps_std, templ->h265_dec.sps_std_count);
         params->h265_dec.sps_std_count = templ->h265_dec.sps_std_count;
         typed_memcpy(params->h265_dec.pps_std, templ->h265_dec.pps_std, templ->h265_dec.pps_std_count);
         params->h265_dec.pps_std_count = templ->h265_dec.pps_std_count;
      } else {
         params->h265_dec.sps_std_count = 0;
         params->h265_dec.pps_std_count = 0;
      }

      if (h265_create->pParametersAddInfo)
         init_add_h265_session_parameters(params, h265_create->pParametersAddInfo);
   }
   default:
      break;
   }
   return VK_SUCCESS;
}

static VkResult
update_h264_session_parameters(struct vk_video_session_parameters *params,
                               const struct VkVideoDecodeH264SessionParametersAddInfoEXT *h264_add)
{
   VkResult result = VK_SUCCESS;
   if (params->h264_dec.sps_std_count + h264_add->spsStdCount >= params->h264_dec.max_sps_std_count)
      result = VK_ERROR_TOO_MANY_OBJECTS;
   else {
      typed_memcpy(&params->h264_dec.sps_std[params->h264_dec.sps_std_count], h264_add->pSpsStd,
                   h264_add->spsStdCount);
      params->h264_dec.sps_std_count += h264_add->spsStdCount;
   }

   if (params->h264_dec.pps_std_count + h264_add->ppsStdCount >= params->h264_dec.max_pps_std_count)
      result = VK_ERROR_TOO_MANY_OBJECTS;
   else {
      typed_memcpy(&params->h264_dec.pps_std[params->h264_dec.pps_std_count], h264_add->pPpsStd,
                   h264_add->ppsStdCount);
      params->h264_dec.pps_std_count += h264_add->ppsStdCount;
   }
   return result;
}

static VkResult
update_h265_session_parameters(struct vk_video_session_parameters *params,
                               const struct VkVideoDecodeH265SessionParametersAddInfoEXT *h265_add)
{
   VkResult result = VK_SUCCESS;
   if (params->h265_dec.sps_std_count + h265_add->spsStdCount >= params->h265_dec.max_sps_std_count)
      result = VK_ERROR_TOO_MANY_OBJECTS;
   else {
      typed_memcpy(&params->h265_dec.sps_std[params->h265_dec.sps_std_count], h265_add->pSpsStd,
                   h265_add->spsStdCount);
      params->h265_dec.sps_std_count += h265_add->spsStdCount;
   }

   if (params->h265_dec.pps_std_count + h265_add->ppsStdCount >= params->h265_dec.max_pps_std_count)
      result = VK_ERROR_TOO_MANY_OBJECTS;
   else {
      typed_memcpy(&params->h265_dec.pps_std[params->h265_dec.pps_std_count], h265_add->pPpsStd,
                   h265_add->ppsStdCount);
      params->h265_dec.pps_std_count += h265_add->ppsStdCount;
   }
   return result;
}

VkResult
vk_video_session_parameters_update(struct vk_video_session_parameters *params,
                                   const VkVideoSessionParametersUpdateInfoKHR *update)
{
   /* 39.6.5. Decoder Parameter Sets -
    * "The provided H.264 SPS/PPS parameters must be within the limits specified during decoder
    * creation for the decoder specified in VkVideoSessionParametersCreateInfoKHR."
    */
   VkResult result = VK_SUCCESS;

   switch (params->op) {
   case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_EXT: {
      const struct VkVideoDecodeH264SessionParametersAddInfoEXT *h264_add =
         vk_find_struct_const(update->pNext, VIDEO_DECODE_H264_SESSION_PARAMETERS_ADD_INFO_EXT);
      return update_h264_session_parameters(params, h264_add);
   }
   case VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_EXT: {
      const struct VkVideoDecodeH265SessionParametersAddInfoEXT *h265_add =
         vk_find_struct_const(update->pNext, VIDEO_DECODE_H265_SESSION_PARAMETERS_ADD_INFO_EXT);

      return update_h265_session_parameters(params, h265_add);
   }
   default:
      unreachable("Unknown codec\n");
   }
   return result;
}

const StdVideoH264SequenceParameterSet *
vk_video_find_h264_sps(const struct vk_video_session_parameters *params,
                       uint32_t sps_id)
{
   for (unsigned i = 0; i < params->h264_dec.sps_std_count; i++) {
      if (params->h264_dec.sps_std[i].seq_parameter_set_id == sps_id)
         return &params->h264_dec.sps_std[i];
   }
   return NULL;
}

const StdVideoH264PictureParameterSet *
vk_video_find_h264_pps(const struct vk_video_session_parameters *params,
                       uint32_t pps_id)
{
   for (unsigned i = 0; i < params->h264_dec.pps_std_count; i++) {
      if (params->h264_dec.pps_std[i].pic_parameter_set_id == pps_id)
         return &params->h264_dec.pps_std[i];
   }
   return NULL;
}

void
vk_video_session_parameters_finish(struct vk_device *device,
                                   struct vk_video_session_parameters *params)
{
   switch (params->op) {
   case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_EXT:
      vk_free(&device->alloc, params->h264_dec.sps_std);
      vk_free(&device->alloc, params->h264_dec.pps_std);
      break;
   case VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_EXT:
      vk_free(&device->alloc, params->h265_dec.sps_std);
      vk_free(&device->alloc, params->h265_dec.pps_std);
      break;
   default:
      break;
   }
   vk_object_base_finish(&params->base);
}

static void
ref_pic_list_mod(struct vl_rbsp *rbsp,
                 struct vk_video_h264_slice_params *params)
{
   unsigned modification_of_pic_nums_idc;
   int idx = 0;
   if (params->slice_type != STD_VIDEO_H264_SLICE_TYPE_I) {
      /* ref_pic_list_modification_flag_l0 */
      if (vl_rbsp_u(rbsp, 1)) {
         do {
            modification_of_pic_nums_idc = vl_rbsp_ue(rbsp);
            if (modification_of_pic_nums_idc < 3) {
               params->mod_pic_nums_idc_l0[idx] = modification_of_pic_nums_idc;
               /* abs_diff_pic_num_minus1 */
               /* long_term_pic_num */
               params->mod_val_l0[idx] = vl_rbsp_ue(rbsp);
               idx++;
            }
         } while (modification_of_pic_nums_idc != 3);
      }
   }
   params->num_mod_l0 = idx;

   idx = 0;
   if (params->slice_type == STD_VIDEO_H264_SLICE_TYPE_B) {
      /* ref_pic_list_modification_flag_l1 */
      if (vl_rbsp_u(rbsp, 1)) {
         do {
            modification_of_pic_nums_idc = vl_rbsp_ue(rbsp);
            if (modification_of_pic_nums_idc < 3) {
               params->mod_pic_nums_idc_l1[idx] = modification_of_pic_nums_idc;
               /* abs_diff_pic_num_minus1 */
               /* long_term_pic_num */
               params->mod_val_l1[idx] = vl_rbsp_ue(rbsp);
               idx++;
            }
         } while (modification_of_pic_nums_idc != 3);
      }
   }
   params->num_mod_l1 = idx;
}

/*
 * Fill out the pred weight table, but fill out the
 * default values if the flags aren't set.
 * This saves the driver filling them out itself later.
 */
static void
pred_weight_table(struct vk_video_h264_slice_params *params,
                  struct vl_rbsp *rbsp,
                  const StdVideoH264SequenceParameterSet *sps,
                  StdVideoH264SliceType slice_type)
{
   unsigned ChromaArrayType = sps->flags.separate_colour_plane_flag ? 0 : sps->chroma_format_idc;
   unsigned i, j;

   /* luma_log2_weight_denom */
   params->luma_log2_weight_denom = vl_rbsp_ue(rbsp);

   if (ChromaArrayType != 0)
      /* chroma_log2_weight_denom */
      params->chroma_log2_weight_denom = vl_rbsp_ue(rbsp);

   for (i = 0; i <= params->num_ref_idx_l0_active_minus1; ++i) {
      /* luma_weight_l0_flag */
      params->luma_weight_l0_flag[i] = vl_rbsp_u(rbsp, 1);
      if (params->luma_weight_l0_flag[i]) {
         /* luma_weight_l0[i] */
         params->luma_weight_l0[i] = vl_rbsp_se(rbsp);
         /* luma_offset_l0[i] */
         params->luma_offset_l0[i] = vl_rbsp_se(rbsp);
      } else {
         params->luma_weight_l0[i] = 1 << params->luma_log2_weight_denom;
      }
      if (ChromaArrayType != 0) {
         /* chroma_weight_l0_flag */
         params->chroma_weight_l0_flag[i] = vl_rbsp_u(rbsp, 1);
         if (params->chroma_weight_l0_flag[i]) {
            for (j = 0; j < 2; ++j) {
               /* chroma_weight_l0[i][j] */
               params->chroma_weight_l0[i][j] = vl_rbsp_se(rbsp);
               /* chroma_offset_l0[i][j] */
               params->chroma_offset_l0[i][j] = vl_rbsp_se(rbsp);
            }
         } else {
            params->chroma_weight_l0[i][0] = 1 << params->chroma_log2_weight_denom;
            params->chroma_weight_l0[i][1] = 1 << params->chroma_log2_weight_denom;
         }
      }
   }

   if (slice_type == STD_VIDEO_H264_SLICE_TYPE_B) {
      for (i = 0; i <= params->num_ref_idx_l1_active_minus1; ++i) {
         /* luma_weight_l1_flag */
         params->luma_weight_l1_flag[i] = vl_rbsp_u(rbsp, 1);
         if (params->luma_weight_l1_flag[i]) {
            /* luma_weight_l1[i] */
            params->luma_weight_l1[i] = vl_rbsp_se(rbsp);
            /* luma_offset_l1[i] */
            params->luma_offset_l1[i] = vl_rbsp_se(rbsp);
         }
         if (ChromaArrayType != 0) {
            /* chroma_weight_l1_flag */
            params->chroma_weight_l1_flag[i] = vl_rbsp_u(rbsp, 1);
            if (params->chroma_weight_l1_flag[i]) {
               for (j = 0; j < 2; ++j) {
                  /* chroma_weight_l1[i][j] */
                  params->chroma_weight_l1[i][j] = vl_rbsp_se(rbsp);
                  /* chroma_offset_l1[i][j] */
                  params->chroma_offset_l1[i][j] = vl_rbsp_se(rbsp);
               }
            }
         }
      }
   }
}

static void
dec_ref_pic_marking(struct vl_rbsp *rbsp,
                    bool IdrPicFlag)
{
   unsigned memory_management_control_operation;

   if (IdrPicFlag) {
      /* no_output_of_prior_pics_flag */
      vl_rbsp_u(rbsp, 1);
      /* long_term_reference_flag */
      vl_rbsp_u(rbsp, 1);
   } else {
      /* adaptive_ref_pic_marking_mode_flag */
      if (vl_rbsp_u(rbsp, 1)) {
         do {
            memory_management_control_operation = vl_rbsp_ue(rbsp);

            if (memory_management_control_operation == 1 ||
                memory_management_control_operation == 3)
               /* difference_of_pic_nums_minus1 */
               vl_rbsp_ue(rbsp);

            if (memory_management_control_operation == 2)
               /* long_term_pic_num */
               vl_rbsp_ue(rbsp);

            if (memory_management_control_operation == 3 ||
                memory_management_control_operation == 6)
               /* long_term_frame_idx */
               vl_rbsp_ue(rbsp);

            if (memory_management_control_operation == 4)
               /* max_long_term_frame_idx_plus1 */
               vl_rbsp_ue(rbsp);
         } while (memory_management_control_operation != 0);
      }
   }
}

void
vk_video_parse_h264_slice_header(const struct VkVideoDecodeInfoKHR *frame_info,
                                 const StdVideoH264SequenceParameterSet *sps,
                                 const StdVideoH264PictureParameterSet *pps,
                                 void *slice_hdr,
                                 struct vk_video_h264_slice_params *params)
{
   struct vl_vlc vlc;
   unsigned sizes = frame_info->srcBufferRange;
   const void *slice_hdrs[1] = { slice_hdr };
   vl_vlc_init(&vlc, 1, slice_hdrs, &sizes);

   assert(vl_vlc_peekbits(&vlc, 24) == 0x000001);

   vl_vlc_eatbits(&vlc, 24);

   /* forbidden_zero_bit */
   vl_vlc_eatbits(&vlc, 1);

   unsigned nal_ref_idc = vl_vlc_get_uimsbf(&vlc, 2);
   unsigned nal_unit_type = vl_vlc_get_uimsbf(&vlc, 5);

   assert(nal_unit_type == 1 || nal_unit_type == 5);

   const uint8_t *orig_ptr = vlc.data;
   struct vl_rbsp rbsp;
   vl_rbsp_init(&rbsp, &vlc, 128);

   memset(params, 0, sizeof(*params));
   /* first_mb_in_slice */
   params->first_mb_in_slice = vl_rbsp_ue(&rbsp);
   params->slice_type = vl_rbsp_ue(&rbsp) % 5;
   vl_rbsp_ue(&rbsp);//pps id

   if (sps->flags.separate_colour_plane_flag)
      vl_rbsp_u(&rbsp, 2);

   vl_rbsp_u(&rbsp, sps->log2_max_frame_num_minus4 + 4);
   unsigned field_pic_flag = 0;
   if (!sps->flags.frame_mbs_only_flag) {
      field_pic_flag = vl_rbsp_u(&rbsp, 1);
      if (field_pic_flag)
         /*unsigned bottom_field_flag =*/ vl_rbsp_u(&rbsp, 1);
   }

   if (nal_unit_type == 5) {
      /*unsigned idr_pic_id =*/ vl_rbsp_ue(&rbsp);
   }
   if (sps->pic_order_cnt_type == 0) {
      unsigned log2_max_pic_order_cnt_lsb = sps->log2_max_pic_order_cnt_lsb_minus4 + 4;
      /* unsigned pic_order_cnt_lsb */ vl_rbsp_u(&rbsp, log2_max_pic_order_cnt_lsb);
      if (0) {//pps->flags.bottom_field_pic_order_in_frame_present_flag && !field_pic_flag) {
         /* delta_pic-order_cnt[1] */ vl_rbsp_se(&rbsp);
      }
   }

   if (pps->flags.redundant_pic_cnt_present_flag)
      /* redundant_pic_cnt */
     vl_rbsp_ue(&rbsp);

   if (params->slice_type == STD_VIDEO_H264_SLICE_TYPE_B)
      /* direct_spatial_mv_pred_flag */
      params->direct_spatial_mv_pred_flag = vl_rbsp_u(&rbsp, 1);

   params->num_ref_idx_l0_active_minus1 = pps->num_ref_idx_l0_default_active_minus1;
   params->num_ref_idx_l1_active_minus1 = pps->num_ref_idx_l1_default_active_minus1;

   if (params->slice_type != STD_VIDEO_H264_SLICE_TYPE_B)
      params->num_ref_idx_l1_active_minus1 = 0;
   if (params->slice_type != STD_VIDEO_H264_SLICE_TYPE_I) {
      /* num_ref_idx_active_override_flag */
      if (vl_rbsp_u(&rbsp, 1)) {
         params->num_ref_idx_l0_active_minus1 = vl_rbsp_ue(&rbsp);

         if (params->slice_type == STD_VIDEO_H264_SLICE_TYPE_B)
            params->num_ref_idx_l1_active_minus1 = vl_rbsp_ue(&rbsp);
      }
   } else
      params->num_ref_idx_l0_active_minus1 = 0;

   if (nal_unit_type == 20 || nal_unit_type == 21)
      assert(0);
   else
      ref_pic_list_mod(&rbsp, params);

   if ((pps->flags.weighted_pred_flag && params->slice_type == STD_VIDEO_H264_SLICE_TYPE_P) ||
       (pps->weighted_bipred_idc == 1 && params->slice_type == STD_VIDEO_H264_SLICE_TYPE_B))
      pred_weight_table(params, &rbsp, sps, params->slice_type);

   if (nal_ref_idc != 0)
      dec_ref_pic_marking(&rbsp, nal_unit_type == 5);

   if (pps->flags.entropy_coding_mode_flag &&
       params->slice_type != STD_VIDEO_H264_SLICE_TYPE_I)
      /* cabac_init_idc */
      params->cabac_init_idc = vl_rbsp_ue(&rbsp);

   /* slice_qp_delta */
   params->slice_qp_delta = vl_rbsp_se(&rbsp);

   /* Spec decodes some SP/SI attributes.
    * SP slice sp_for_switch_flag vl_rbsp_u
    * SP/SI slice_qs_delta. vl_rbsp_se
    */

   if (pps->flags.deblocking_filter_control_present_flag) {
      params->disable_deblocking_filter_idc = vl_rbsp_ue(&rbsp);

      if (params->disable_deblocking_filter_idc != 1) {
         /* slice_alpha_c0_offset_div2 */
         params->slice_alpha_c0_offset_div2 = vl_rbsp_se(&rbsp);

         /* slice_beta_offset_div2 */
         params->slice_beta_offset_div2 = vl_rbsp_se(&rbsp);
      }
   }

   /* Vulkan will never give us num_slice_group_minus1 > 0 so no need to worry about slice_group_change_cycle */

   /*
    * This is a bit horrible. - the rbsp overfetches, so workout how much data
    * it has consumed, then depending on the direction, remove or add back the
    * bits it hasn't consumed. Add back the 8-bits from the original NAL header.
    */
   params->slice_data_bit_offset = ((rbsp.nal.data - orig_ptr) * 8) +
      rbsp.nal.invalid_bits - ((rbsp.nal.invalid_bits < 0) ? 32 : 0) + 8;
}

void
vk_fill_video_reference_info(const VkVideoDecodeInfoKHR *frame_info,
                             struct vk_video_h264_reference *ref_slots)
{
   for (unsigned i = 0; i < frame_info->referenceSlotCount; i++) {
      const VkVideoDecodeH264DpbSlotInfoEXT *dpb_slot_info = vk_find_struct_const(frame_info->pReferenceSlots[i].pNext, VIDEO_DECODE_H264_DPB_SLOT_INFO_EXT);
      ref_slots[i].pPictureResource = frame_info->pReferenceSlots[i].pPictureResource;
      ref_slots[i].slot_index = frame_info->pReferenceSlots[i].slotIndex;

      ref_slots[i].frame_num = dpb_slot_info->pStdReferenceInfo->FrameNum;
      ref_slots[i].flags = dpb_slot_info->pStdReferenceInfo->flags;
      ref_slots[i].pic_order_cnt[0] = dpb_slot_info->pStdReferenceInfo->PicOrderCnt[0];
      ref_slots[i].pic_order_cnt[1] = dpb_slot_info->pStdReferenceInfo->PicOrderCnt[1];
   }
}

/* H.264 - 8.2.4.2.1 Initialisation process for the reference picture list
 *  for P and SP slices in frames
 * P frames are sorted by frame num.
 */
void
vk_video_sort_p_ref_frames(uint32_t count,
                           const struct vk_video_h264_reference *refs,
                           const struct vk_video_h264_slice_params *params,
                           int32_t curr_frame_num,
                           int32_t max_frame_num,
                           int32_t *sorted_idxs)
{
   int32_t sorted_fn_refs[32];

   /* sort the list by descending frame number */
   for (unsigned i = 0; i < count; i++) {
      int found_idx = -1;
      int to_find = -1;
      int limit;
      if (i > 0)
         limit = sorted_fn_refs[i - 1];
      else
         limit = INT_MAX;

      for (unsigned j = 0; j < count; j++) {
         if (refs[j].frame_num > to_find && refs[j].frame_num < limit) {
            to_find = refs[j].frame_num;
            found_idx = j;
         }
      }

      if (found_idx >= 0 && found_idx < count) {
         sorted_idxs[i] = found_idx;
         sorted_fn_refs[i] = to_find;
      }
   }

   if (params->num_mod_l0 > 0) {
      for (unsigned m = 0; m < params->num_mod_l0; m++) {
         int op = params->mod_pic_nums_idc_l0[m];

         switch (op) {
         case 0:
         case 1: {
            unsigned new_pic_num = curr_frame_num;
            unsigned abs_diff_pic_num = params->mod_val_l0[m] + 1;
            if (op == 0)
               new_pic_num -= abs_diff_pic_num;
            else
               new_pic_num += abs_diff_pic_num;
            new_pic_num &= max_frame_num - 1;

            /* find the new pic num */
            unsigned i;
            for (i = 0; i < count; i++) {
               if (refs[sorted_idxs[i]].frame_num == new_pic_num) {
                  break;
               }
            }
            if (i < count) {
               int found_idx = sorted_idxs[i];
               /* move all the higher index down one. */
               memmove(&sorted_idxs[i], &sorted_idxs[i + 1], (count - i - 1) * sizeof(int32_t));
               memmove(&sorted_idxs[m + 1], &sorted_idxs[m], (count - m - 1) * sizeof(int32_t));
               sorted_idxs[m] = found_idx;
            }
            curr_frame_num = new_pic_num;
            break;
         }
         case 2:
         default:
            assert(0);
         }
      }
   }
}

static int
do_descend_sort(const struct vk_video_h264_reference *refs,
                int32_t count,
                int32_t from, int32_t to,
                int32_t *sorted_poc_idxs)
{
   int len = 0;
   int32_t sorted_poc_refs[32];
   /* sort the list by descending poc number for values under curr_poc */
   for (unsigned i = 0; i < count; i++) {
      int found_idx = -1;
      int limit;

      int to_find = from;
      if (len > 0)
         limit = sorted_poc_refs[len - 1];
      else
         limit = to;

      /* find the first highest pic order cnt < curr_poc that hasn't
         been processed */
      for (unsigned j = 0; j < count; j++) {
         if (refs[j].pic_order_cnt[0] > to_find && refs[j].pic_order_cnt[0] < limit) {
            to_find = refs[j].pic_order_cnt[0];
            found_idx = j;
         }
      }

      if (found_idx >= 0 && found_idx < count) {
         sorted_poc_idxs[len] = found_idx;
         sorted_poc_refs[len] = to_find;
         len++;
      }
   }
   return len;
}

static int
do_ascend_sort(const struct vk_video_h264_reference *refs,
               int32_t count,
               int32_t from, int32_t to,
               int32_t *sorted_poc_idxs)
{
   int len = 0;
   int32_t sorted_poc_refs[32];

   for (unsigned i = 0; i < count; i++) {
      int found_idx = -1;
      int limit;

      int to_find = to;
      if (len > 0)
         limit = sorted_poc_refs[len - 1];
      else
         limit = from;

      for (unsigned j = 0; j < count; j++) {
         if (refs[j].pic_order_cnt[0] < to_find && refs[j].pic_order_cnt[0] > limit) {
            to_find = refs[j].pic_order_cnt[0];
            found_idx = j;
         }
      }

      if (found_idx >= 0 && found_idx < count) {
         sorted_poc_idxs[len] = found_idx;
         sorted_poc_refs[len] = to_find;
         len++;
      }
   }
   return len;
}

/* H.264 8.2.4.2.3 Initialisation process for reference picture lists for B slices in frames */
int
vk_video_sort_b_l0_ref_frames(uint32_t count,
                              uint32_t curr_poc,
                              const struct vk_video_h264_reference *refs,
                              int32_t *sorted_poc_idxs)
{
   int len;

   len = do_descend_sort(refs, count, INT_MIN, curr_poc, sorted_poc_idxs);
   return do_ascend_sort(refs, count, curr_poc, INT_MAX, sorted_poc_idxs + len);
}

int
vk_video_sort_b_l1_ref_frames(uint32_t count,
                              uint32_t curr_poc,
                              const struct vk_video_h264_reference *refs,
                              int32_t *sorted_poc_idxs)
{
   int len;
   len = do_ascend_sort(refs, count, curr_poc, INT_MAX, sorted_poc_idxs);
   return do_descend_sort(refs, count, INT_MIN, curr_poc, sorted_poc_idxs + len);
}
#endif
