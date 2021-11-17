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

VkResult
vk_video_session_parameters_init(struct vk_device *device,
                                 struct vk_video_session_parameters *params,
                                 const struct vk_video_session *vid,
                                 const VkVideoSessionParametersCreateInfoKHR *create_info)
{
   vk_object_base_init(device, &params->base, VK_OBJECT_TYPE_VIDEO_SESSION_PARAMETERS_KHR);

   params->op = vid->op;

   switch (vid->op) {
   case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_EXT: {
      const struct VkVideoDecodeH264SessionParametersCreateInfoEXT *h264_create =
         vk_find_struct_const(create_info->pNext, VIDEO_DECODE_H264_SESSION_PARAMETERS_CREATE_INFO_EXT);
      params->h264_dec.max_sps_std_count = h264_create->maxSpsStdCount;
      params->h264_dec.max_pps_std_count = h264_create->maxPpsStdCount;

      params->h264_dec.sps_std_count = h264_create->pParametersAddInfo->spsStdCount;
      params->h264_dec.pps_std_count = h264_create->pParametersAddInfo->ppsStdCount;

      uint32_t sps_size = params->h264_dec.max_sps_std_count * sizeof(StdVideoH264SequenceParameterSet);
      uint32_t pps_size = params->h264_dec.max_pps_std_count * sizeof(StdVideoH264PictureParameterSet);

      params->h264_dec.sps_std = vk_alloc(&device->alloc, sps_size, 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      params->h264_dec.pps_std = vk_alloc(&device->alloc, pps_size, 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);

      if (!params->h264_dec.sps_std || !params->h264_dec.pps_std) {
         vk_free(&device->alloc, params->h264_dec.sps_std);
         vk_free(&device->alloc, params->h264_dec.pps_std);
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      }
      typed_memcpy(params->h264_dec.sps_std, h264_create->pParametersAddInfo->pSpsStd,
                   params->h264_dec.sps_std_count);
      typed_memcpy(params->h264_dec.pps_std, h264_create->pParametersAddInfo->pPpsStd,
                   params->h264_dec.pps_std_count);
      break;
   }
   case VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_EXT: {
      const struct VkVideoDecodeH265SessionParametersCreateInfoEXT *h265_create =
         vk_find_struct_const(create_info->pNext, VIDEO_DECODE_H265_SESSION_PARAMETERS_CREATE_INFO_EXT);
      params->h265_dec.max_sps_std_count = h265_create->maxSpsStdCount;
      params->h265_dec.max_pps_std_count = h265_create->maxPpsStdCount;

      params->h265_dec.sps_std_count = h265_create->pParametersAddInfo->spsStdCount;
      params->h265_dec.pps_std_count = h265_create->pParametersAddInfo->ppsStdCount;

      uint32_t sps_size = params->h265_dec.max_sps_std_count * sizeof(StdVideoH265SequenceParameterSet);
      uint32_t pps_size = params->h265_dec.max_pps_std_count * sizeof(StdVideoH265PictureParameterSet);

      params->h265_dec.sps_std = vk_alloc(&device->alloc, sps_size, 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      params->h265_dec.pps_std = vk_alloc(&device->alloc, pps_size, 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);

      if (!params->h265_dec.sps_std || !params->h265_dec.pps_std) {
         vk_free(&device->alloc, params->h265_dec.sps_std);
         vk_free(&device->alloc, params->h265_dec.pps_std);
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      }
      typed_memcpy(params->h265_dec.sps_std, h265_create->pParametersAddInfo->pSpsStd,
                   params->h265_dec.sps_std_count);
      typed_memcpy(params->h265_dec.pps_std, h265_create->pParametersAddInfo->pPpsStd,
                   params->h265_dec.pps_std_count);
   }
   default:
      break;
   }
   return VK_SUCCESS;
}

void
vk_video_session_parameters_update(struct vk_video_session_parameters *params,
                                   const VkVideoSessionParametersUpdateInfoKHR *update)
{
   switch (params->op) {
   case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_EXT: {
      const struct VkVideoDecodeH264SessionParametersAddInfoEXT *h264_add =
         vk_find_struct_const(update->pNext, VIDEO_DECODE_H264_SESSION_PARAMETERS_CREATE_INFO_EXT);

      params->h264_dec.sps_std_count = h264_add->spsStdCount;
      params->h264_dec.pps_std_count = h264_add->ppsStdCount;

      typed_memcpy(params->h264_dec.sps_std, h264_add->pSpsStd,
                   params->h264_dec.sps_std_count);
      typed_memcpy(params->h264_dec.pps_std, h264_add->pPpsStd,
                   params->h264_dec.pps_std_count);
      break;
   }
   case VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_EXT: {
      const struct VkVideoDecodeH265SessionParametersAddInfoEXT *h265_add =
         vk_find_struct_const(update->pNext, VIDEO_DECODE_H265_SESSION_PARAMETERS_CREATE_INFO_EXT);
      params->h265_dec.sps_std_count = h265_add->spsStdCount;
      params->h265_dec.pps_std_count = h265_add->ppsStdCount;

      typed_memcpy(params->h265_dec.sps_std, h265_add->pSpsStd,
                   params->h265_dec.sps_std_count);

      typed_memcpy(params->h265_dec.pps_std, h265_add->pPpsStd,
                   params->h265_dec.pps_std_count);
      break;
   }
   default:
      break;
   }
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
                 StdVideoH264SliceType slice_type)
{
   unsigned modification_of_pic_nums_idc;

   if (slice_type != STD_VIDEO_H264_SLICE_TYPE_I &&
       slice_type != STD_VIDEO_H264_SLICE_TYPE_SI) {
      /* ref_pic_list_modification_flag_l0 */
      if (vl_rbsp_u(rbsp, 1)) {
         do {
            modification_of_pic_nums_idc = vl_rbsp_ue(rbsp);
            if (modification_of_pic_nums_idc == 0 ||
                modification_of_pic_nums_idc == 1)
               /* abs_diff_pic_num_minus1 */
               vl_rbsp_ue(rbsp);
            else if (modification_of_pic_nums_idc == 2)
               /* long_term_pic_num */
               vl_rbsp_ue(rbsp);
         } while (modification_of_pic_nums_idc != 3);
      }
   }

   if (slice_type == STD_VIDEO_H264_SLICE_TYPE_B) {
      /* ref_pic_list_modification_flag_l1 */
      if (vl_rbsp_u(rbsp, 1)) {
         do {
            modification_of_pic_nums_idc = vl_rbsp_ue(rbsp);
            if (modification_of_pic_nums_idc == 0 ||
                modification_of_pic_nums_idc == 1)
               /* abs_diff_pic_num_minus1 */
               vl_rbsp_ue(rbsp);
            else if (modification_of_pic_nums_idc == 2)
               /* long_term_pic_num */
               vl_rbsp_ue(rbsp);
         } while (modification_of_pic_nums_idc != 3);
      }
   }
}

static void
pred_weight_table(struct vk_video_h264_slice_params *params,
                  struct vl_rbsp *rbsp,
                  const StdVideoH264SequenceParameterSet *sps,
                  StdVideoH264SliceType slice_type)
{
   unsigned ChromaArrayType = sps->chroma_format_idc;//sps->separate_colour_plane_flag ? 0 : sps->chroma_format_idc;
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
      } else
         assert(0);
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
      ref_pic_list_mod(&rbsp, params->slice_type);

   if ((pps->flags.weighted_pred_flag && (params->slice_type == STD_VIDEO_H264_SLICE_TYPE_P || params->slice_type == STD_VIDEO_H264_SLICE_TYPE_SP)) ||
       (pps->weighted_bipred_idc == 1 && params->slice_type == STD_VIDEO_H264_SLICE_TYPE_B))
      pred_weight_table(params, &rbsp, sps, params->slice_type);

   if (nal_ref_idc != 0)
      dec_ref_pic_marking(&rbsp, nal_unit_type == 5);

   if (pps->flags.entropy_coding_mode_flag &&
       params->slice_type != STD_VIDEO_H264_SLICE_TYPE_I &&
       params->slice_type != STD_VIDEO_H264_SLICE_TYPE_SI)
      /* cabac_init_idc */
      params->cabac_init_idc = vl_rbsp_ue(&rbsp);

   /* slice_qp_delta */
   params->slice_qp_delta = vl_rbsp_se(&rbsp);

   if (params->slice_type == STD_VIDEO_H264_SLICE_TYPE_SP ||
       params->slice_type == STD_VIDEO_H264_SLICE_TYPE_SI) {
      if (params->slice_type == STD_VIDEO_H264_SLICE_TYPE_SP)
         /* sp_for_switch_flag */
         vl_rbsp_u(&rbsp, 1);

      /*slice_qs_delta */
      vl_rbsp_se(&rbsp);
   }

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

   /* this is a bit horrible. - the rbsp overfetches, so workout how much data it has consumed, then depending
      on the direction, remove or add back the bits it hasn't consumed. Add back the 8-bits from the original
      NAL header. */
   params->slice_data_bit_offset = ((rbsp.nal.data - orig_ptr) * 8) + rbsp.nal.invalid_bits - ((rbsp.nal.invalid_bits < 0) ? 32 : 0) + 8;
}

/* p frames are sorted by frame num */
void vk_video_sort_p_ref_frames(uint32_t count,
                                const struct vk_video_h264_reference *refs,
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
}

/* pic list 0 -
   pic_order_cnt < curr_poc - descending order
   then
   pic_order_cnt > curr_poc - ascending order.

   pic list 1 -
   pic_order_cnt > curr_poc - ascending order
   then
   pic_order_cnt < curr_poc - descending order.
*/

static int do_descend_sort(const struct vk_video_h264_reference *refs,
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

      /* find the first highest pic order cnt < curr_poc that hasn't been processed */
      for (unsigned j = 0; j < count; j++) {
         if (refs[j].pic_order_cnt[0] > to_find && refs[j].pic_order_cnt[0] < limit) {
            to_find = refs[j].pic_order_cnt[0];
            found_idx = j;
         }
      }

      if (found_idx >= 0 && found_idx < count) {
         fprintf(stderr, "descend %d %d - %d %d\n", from, to, len, to_find);
         sorted_poc_idxs[len] = found_idx;
         sorted_poc_refs[len] = to_find;
         len++;
      }
   }
   return len;
}

static int do_ascend_sort(const struct vk_video_h264_reference *refs,
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
         fprintf(stderr, "ascend %d %d - %d %d\n", from, to, len, to_find);
         sorted_poc_idxs[len] = found_idx;
         sorted_poc_refs[len] = to_find;
         len++;
      }
   }
   return len;
}


int vk_video_sort_b_l0_ref_frames(uint32_t count,
                                  uint32_t curr_poc,
                                  const struct vk_video_h264_reference *refs,
                                  int32_t *sorted_poc_idxs)
{
   int len;

   len = do_descend_sort(refs, count, INT_MIN, curr_poc, sorted_poc_idxs);
   return do_ascend_sort(refs, count, curr_poc, INT_MAX, sorted_poc_idxs + len);
}

int vk_video_sort_b_l1_ref_frames(uint32_t count,
                                  uint32_t curr_poc,
                                  const struct vk_video_h264_reference *refs,
                                  int32_t *sorted_poc_idxs)
{
   int len;
   len = do_ascend_sort(refs, count, curr_poc, INT_MAX, sorted_poc_idxs);
   return do_descend_sort(refs, count, INT_MIN, curr_poc, sorted_poc_idxs + len);
}

void vk_fill_video_reference_info(const VkVideoDecodeInfoKHR *frame_info,
                                  struct vk_video_h264_reference ref_slots[32])
{
   for (unsigned i = 0; i < frame_info->referenceSlotCount; i++) {
      const VkVideoDecodeH264DpbSlotInfoEXT *dpb_slot_info = vk_find_struct_const(frame_info->pReferenceSlots[i].pNext, VIDEO_DECODE_H264_DPB_SLOT_INFO_EXT);
      ref_slots[i].pPictureResource = frame_info->pReferenceSlots[i].pPictureResource;

      ref_slots[i].frame_num = dpb_slot_info->pStdReferenceInfo->FrameNum;
      ref_slots[i].flags = dpb_slot_info->pStdReferenceInfo->flags;
      ref_slots[i].pic_order_cnt[0] = dpb_slot_info->pStdReferenceInfo->PicOrderCnt[0];
      ref_slots[i].pic_order_cnt[1] = dpb_slot_info->pStdReferenceInfo->PicOrderCnt[1];
   };
}
#endif
