#ifndef VULKAN_VIDEO_CODEC_AV1STD_H_
#define VULKAN_VIDEO_CODEC_AV1STD_H_ 1


/*
** Copyright 2015-2022 The Khronos Group Inc.
**
** SPDX-License-Identifier: Apache-2.0
*/

/*
** This header is NOT YET generated from the Khronos Vulkan XML API Registry.
**
*/

#ifdef __cplusplus
extern "C" {
#endif
#define vulkan_video_codec_av1std 1

typedef enum StdVideoAV1Profile {
   STD_VIDEO_AV1_PROFILE_MAIN = 0,
   STD_VIDEO_AV1_PROFILE_HIGH = 1,
   STD_VIDEO_AV1_PROFILE_PROFESSIONAL = 2,
} StdVideoAV1Profile;

typedef enum StdVideoAV1Level {
    STD_VIDEO_AV1_LEVEL_2_0 = 0,
    STD_VIDEO_AV1_LEVEL_2_1 = 1,
    STD_VIDEO_AV1_LEVEL_2_2 = 2,
    STD_VIDEO_AV1_LEVEL_2_3 = 3,
    STD_VIDEO_AV1_LEVEL_3_0 = 4,
    STD_VIDEO_AV1_LEVEL_3_1 = 5,
    STD_VIDEO_AV1_LEVEL_3_2 = 6,
    STD_VIDEO_AV1_LEVEL_3_3 = 7,
    STD_VIDEO_AV1_LEVEL_4_0 = 8,
    STD_VIDEO_AV1_LEVEL_4_1 = 9,
    STD_VIDEO_AV1_LEVEL_4_2 = 10,
    STD_VIDEO_AV1_LEVEL_4_3 = 11,
    STD_VIDEO_AV1_LEVEL_5_0 = 12,
    STD_VIDEO_AV1_LEVEL_5_1 = 13,
    STD_VIDEO_AV1_LEVEL_5_2 = 14,
    STD_VIDEO_AV1_LEVEL_5_3 = 15,
    STD_VIDEO_AV1_LEVEL_6_0 = 16,
    STD_VIDEO_AV1_LEVEL_6_1 = 17,
    STD_VIDEO_AV1_LEVEL_6_2 = 18,
    STD_VIDEO_AV1_LEVEL_6_3 = 19,
    STD_VIDEO_AV1_LEVEL_7_0 = 20,
    STD_VIDEO_AV1_LEVEL_7_1 = 21,
    STD_VIDEO_AV1_LEVEL_7_2 = 22,
    STD_VIDEO_AV1_LEVEL_7_3 = 23,
    STD_VIDEO_AV1_LEVEL_MAX = 31,
} StdVideoAV1Level;

typedef enum StdVideoAV1TransformationType {
   STD_VIDEO_AV1_TRANSFORMATION_IDENTITY = 0,
   STD_VIDEO_AV1_TRANSFORMATION_TRANSLATION = 1,
   STD_VIDEO_AV1_TRANSFORMATION_ROTZOOM = 2,
   STD_VIDEO_AV1_TRANSFORMATION_AFFINE = 3,
} StdVideoAV1TransformationType;

typedef struct StdVideoAV1SequenceInfoFlags {
   uint32_t still_picture : 1;
   uint32_t use_128x128_superblock : 1;
   uint32_t enable_filter_intra : 1;
   uint32_t enable_intra_edge_filter : 1;
   uint32_t enable_interintra_compound : 1;
   uint32_t enable_masked_compound : 1;
   uint32_t enable_warped_motion : 1;
   uint32_t enable_dual_filter : 1;
   uint32_t enable_order_hint : 1;
   uint32_t enable_jnt_comp : 1;
   uint32_t enable_ref_frame_mvs : 1;
   uint32_t enable_superres : 1;
   uint32_t enable_cdef : 1;
   uint32_t enable_restoration : 1;
   uint32_t film_grain_params_present : 1;
} StdVideoAV1SequenceInfoFlags;

typedef struct StdVideoAV1LoopRestorationParameterSet {
   uint16_t yframe_restoration_type : 2;
   uint16_t cbframe_restoration_type : 2;
   uint16_t crframe_restoration_type : 2;
   uint16_t lr_unit_shift : 2;
   uint16_t lr_uv_shift : 1;
} StdVideoAV1LoopRestorationParameterSet;

typedef struct StdVideoAV1WarpedMotionParameters {
   StdVideoAV1TransformationType wm_type;
   int32_t wm_mat[8];
   VkBool32 invalid;
} StdVideoAV1WarpedMotionParameters;

typedef struct StdVideoAV1QMatrixParameterSet {
   uint16_t using_qmatrix;
   uint16_t qm_y : 4;
   uint16_t qm_u : 4;
   uint16_t qm_v : 4;
} StdVideoAV1QMatrixParameterSet;

typedef struct StdVideoAV1ModeControlParameterSet {
   uint32_t delta_q_present_flag : 1;
   uint32_t log2_delta_q_res : 2;
   uint32_t delta_lf_present_flag : 1;
   uint32_t log2_delta_lf_res : 2;
   uint32_t delta_lf_multi : 1;
   uint32_t tx_mode : 2;
   uint32_t reference_select : 1;
   uint32_t reduced_tx_set_used : 1;
   uint32_t skip_mode_present : 1;
} StdVideoAV1ModeControlParameterSet;

typedef struct StdVideoAV1PictureControlParameterSet {
   uint32_t frame_type : 2;
   uint32_t error_resilient_mode : 1;
   uint32_t disable_cdf_update : 1;
   uint32_t use_superres : 1;
   uint32_t allow_high_precision_mv : 1;
   uint32_t use_ref_frame_mvs : 1;
   uint32_t disable_frame_end_update_cdf : 1;
} StdVideoAV1PictureControlParameterSet;

typedef struct StdVideoAV1PictureParameterSet {
   uint32_t bit_depth_idx;
   uint8_t matrix_coefficients;
   uint16_t frame_width_minus1;
   uint16_t frame_height_minus1;
   uint16_t output_frame_width_in_tiles_minus_1;
   uint16_t output_frame_height_in_tiles_minus_1;

   uint8_t superres_scale_denominator;
   uint8_t interp_filter;
   uint8_t filter_level[2];
   uint8_t filter_level_u;
   uint8_t filter_level_v;

   int8_t ref_deltas[8];
   uint8_t base_qindex;
   int8_t y_dc_delta_q;
   int8_t u_dc_delta_q;
   int8_t u_ac_delta_q;
   int8_t v_dc_delta_q;
   int8_t v_ac_delta_q;

   uint8_t     cdef_damping_minus_3;
   uint8_t     cdef_bits;
   uint8_t     cdef_y_strengths[8];
   uint8_t     cdef_uv_strengths[8];

   StdVideoAV1PictureControlParameterSet picture_control;
   StdVideoAV1ModeControlParameterSet mode_control;
   StdVideoAV1QMatrixParameterSet qmatrix;
   StdVideoAV1LoopRestorationParameterSet loop_restoration;
   StdVideoAV1WarpedMotionParameters wm[7];
} StdVideoAV1PictureParameterSet;


#ifdef __cplusplus
}
#endif

#endif
