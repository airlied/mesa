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
#ifndef VK_VIDEO_H
#define VK_VIDEO_H

#ifdef VK_ENABLE_BETA_EXTENSIONS

#include "vk_object.h"

#ifdef __cplusplus
extern "C" {
#endif

struct vk_video_session {
   struct vk_object_base base;
   VkVideoCodecOperationFlagsKHR op;
   VkExtent2D max_coded;
   VkFormat picture_format;
   VkFormat ref_format;
   uint32_t max_ref_pic_slots;
   uint32_t max_ref_pic_active;

   union {
      struct {
         StdVideoH264ProfileIdc profile_idc;
      } h264;
      struct {
         StdVideoH265ProfileIdc profile_idc;
      } h265;
   };
};

struct vk_video_session_parameters {
   struct vk_object_base base;
   VkVideoCodecOperationFlagsKHR op;
   union {
      struct {
         uint32_t max_sps_std_count;
         uint32_t max_pps_std_count;

         uint32_t sps_std_count;
         StdVideoH264SequenceParameterSet *sps_std;
         uint32_t pps_std_count;
         StdVideoH264PictureParameterSet *pps_std;
      } h264_dec;

      struct {
         uint32_t max_sps_std_count;
         uint32_t max_pps_std_count;

         uint32_t sps_std_count;
         StdVideoH265SequenceParameterSet *sps_std;
         uint32_t pps_std_count;
         StdVideoH265PictureParameterSet *pps_std;
      } h265_dec;
   };
};

VkResult vk_video_session_init(struct vk_device *device,
                               struct vk_video_session *vid,
                               const VkVideoSessionCreateInfoKHR *create_info);

VkResult vk_video_session_parameters_init(struct vk_device *device,
                                          struct vk_video_session_parameters *params,
                                          const struct vk_video_session *vid,
                                          const struct vk_video_session_parameters *templ,
                                          const VkVideoSessionParametersCreateInfoKHR *create_info);

VkResult vk_video_session_parameters_update(struct vk_video_session_parameters *params,
                                            const VkVideoSessionParametersUpdateInfoKHR *update);

void vk_video_session_parameters_finish(struct vk_device *device,
                                        struct vk_video_session_parameters *params);

const StdVideoH264SequenceParameterSet *vk_video_find_h264_sps(const struct vk_video_session_parameters *params, uint32_t sps_id);
const StdVideoH264PictureParameterSet *vk_video_find_h264_pps(const struct vk_video_session_parameters *params, uint32_t pps_id);

struct vk_video_h264_slice_params {
   uint16_t slice_data_bit_offset;
   uint16_t first_mb_in_slice;
   StdVideoH264SliceType slice_type;
   uint8_t direct_spatial_mv_pred_flag;
   uint8_t num_ref_idx_l0_active_minus1;
   uint8_t num_ref_idx_l1_active_minus1;
   uint8_t cabac_init_idc;
   int8_t slice_qp_delta;
   uint8_t disable_deblocking_filter_idc;
   int8_t slice_alpha_c0_offset_div2;
   int8_t slice_beta_offset_div2;
   uint8_t luma_log2_weight_denom;
   uint8_t chroma_log2_weight_denom;
   uint8_t luma_weight_l0_flag[32];
   int16_t luma_weight_l0[32];
   int16_t luma_offset_l0[32];
   uint8_t chroma_weight_l0_flag[32];
   int16_t chroma_weight_l0[32][2];
   int16_t chroma_offset_l0[32][2];
   uint8_t luma_weight_l1_flag[32];
   int16_t luma_weight_l1[32];
   int16_t luma_offset_l1[32];
   uint8_t chroma_weight_l1_flag[32];
   int16_t chroma_weight_l1[32][2];
   int16_t chroma_offset_l1[32][2];

   int16_t num_mod_l0;
   int16_t mod_pic_nums_idc_l0[16];
   int16_t mod_val_l0[16];
   int16_t num_mod_l1;
   int16_t mod_pic_nums_idc_l1[16];
   int16_t mod_val_l1[16];
};

void vk_video_parse_h264_slice_header(const struct VkVideoDecodeInfoKHR *frame_info,
                                      const StdVideoH264SequenceParameterSet *sps,
                                      const StdVideoH264PictureParameterSet *pps,
                                      void *slice_hdr,
                                      struct vk_video_h264_slice_params *params);

struct vk_video_h264_reference {
   const VkVideoPictureResourceKHR *pPictureResource;
   StdVideoDecodeH264ReferenceInfoFlags flags;
   uint32_t slot_index;
   int16_t frame_num;
   int32_t pic_order_cnt[2];
};

/* fill out the needed info without having to chase pNext */
void vk_fill_video_reference_info(const VkVideoDecodeInfoKHR *frame_info,
                                  struct vk_video_h264_reference *ref_slots);

void vk_video_sort_p_ref_frames(uint32_t count,
                                const struct vk_video_h264_reference *refs,
                                const struct vk_video_h264_slice_params *params,
                                int32_t curr_frame_num,
                                int32_t max_frame_num,
                                int32_t *sorted_idxs);
int vk_video_sort_b_l0_ref_frames(uint32_t count,
                                  uint32_t curr_poc,
                                  const struct vk_video_h264_reference *refs,
                                  int32_t *sorted_poc_idxs);
int vk_video_sort_b_l1_ref_frames(uint32_t count,
                                  uint32_t curr_poc,
                                  const struct vk_video_h264_reference *refs,
                                  int32_t *sorted_poc_idxs);

#ifdef __cplusplus
}
#endif

#endif
#endif
