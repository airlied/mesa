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

#define VK_MAKE_VIDEO_STD_VERSION(major, minor, patch) \
   ((((uint32_t)(major)) << 22) | (((uint32_t)(minor)) << 12) | ((uint32_t)(patch)))
#define VK_STD_VULKAN_VIDEO_CODEC_AV1_DECODE_API_VERSION_0_0_1 VK_MAKE_VIDEO_STD_VERSION(0, 0, 1)
#define VK_STD_VULKAN_VIDEO_CODEC_AV1_DECODE_SPEC_VERSION VK_STD_VULKAN_VIDEO_CODEC_AV1_DECODE_API_VERSION_0_0_1
#define VK_STD_VULKAN_VIDEO_CODEC_AV1_DECODE_EXTENSION_NAME "VK_STD_vulkan_video_codec_av1_decode"

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

typedef struct StdVideoAV1FilmGrainFlags {
   uint32_t apply_grain : 1;
   uint32_t chroma_scaling_from_luma : 1;
   uint32_t overlap_flag : 1;
   uint32_t clip_to_restricted_range : 1;
} StdVideoAV1FilmGrainFlags;

typedef struct StdVideoAV1FilmGrainParameters {
   StdVideoAV1FilmGrainFlags flags;
   uint32_t grain_scaling_minus_8;
   uint32_t ar_coeff_lag;
   uint32_t ar_coeff_shift_minus_6;
   uint32_t grain_scale_shift;

   uint16_t grain_seed;
   uint8_t num_y_points;
   uint8_t point_y_value[14];
   uint8_t point_y_scaling[14];

   uint8_t num_cb_points;
   uint8_t point_cb_value[10];
   uint8_t point_cb_scaling[10];

   uint8_t num_cr_points;
   uint8_t point_cr_value[10];
   uint8_t point_cr_scaling[10];

   int8_t ar_coeffs_y_plus_128[24];
   int8_t ar_coeffs_cb_plus_128[25];
   int8_t ar_coeffs_cr_plus_128[25];
   uint8_t cb_mult;
   uint8_t cb_luma_mult;
   uint16_t cb_offset;
   uint8_t cr_mult;
   uint8_t cr_luma_mult;
   uint16_t cr_offset;
} StdVideoAV1FilmGrainParameters;

typedef struct StdVideoAV1WarpedMotionFlags {
    uint8_t is_global : 1;
    uint8_t is_rot_zoom : 1;
    uint8_t is_translation : 1;
} StdVideoAV1WarpedMotionFlags;

typedef struct StdVideoAV1WarpedMotion {
    StdVideoAV1WarpedMotionFlags flags;
    uint32_t gm_params[6];
} StdVideoAV1WarpedMotion;

typedef struct StdVideoAV1LoopRestoration {
    uint8_t lr_type[3];
    uint8_t lr_unit_shift;
    uint8_t lr_uv_shift;
} StdVideoAV1LoopRestoration;

typedef struct StdVideoAV1TilingFlags {
    uint8_t uniform_tile_spacing_flag;
} StdVideoAV1TilingFlags;

typedef struct StdVideoAV1Tiling {
    StdVideoAV1TilingFlags flags;
    uint8_t tile_cols_log2;
    uint8_t tile_rows_log2;
    uint8_t width_in_sbs_minus_1[64];
    uint8_t height_in_sbs_minus_1[64];
    uint16_t context_update_tile_id;
    uint8_t tile_size_bytes_minus1;
} StdVideoAV1Tiling;

typedef struct StdVideoAV1Quantization {
    uint8_t base_q_idx;
    int8_t  delta_q_y_dc;
    uint8_t diff_uv_delta;
    int8_t  delta_q_u_dc;
    int8_t  delta_q_u_ac;
    int8_t  delta_q_v_dc;
    int8_t  delta_q_v_ac;
    uint8_t qm_y;
    uint8_t qm_u;
    uint8_t qm_v;
} StdVideoAV1Quantization;

typedef struct StdVideoAV1CDEF {
    uint8_t cdef_damping_minus_3;
    uint8_t cdef_bits;
    uint8_t cdef_y_pri_strength[8];
    uint8_t cdef_y_sec_strength[8];
    uint8_t cdef_uv_pri_strength[8];
    uint8_t cdef_uv_sec_strength[8];
} StdVideoAV1CDEF;

typedef struct StdVideoAV1DeltaQFlags {
    uint8_t delta_lf_present : 1;
    uint8_t delta_lf_multi : 1;
} StdVideoAV1DeltaQFlags;

typedef struct StdVideoAV1DeltaQ {
    StdVideoAV1DeltaQFlags flags;
    uint8_t delta_q_res;
    uint8_t delta_lf_res;
} StdVideoAV1DeltaQ;

typedef struct StdVideoAV1SegmentationFlags {
    uint32_t segmentation_enabled : 1;
    uint32_t segmentation_update_map : 1;
    uint32_t segmentation_temporal_update : 1;
    uint32_t segmentation_update_data : 1;
} StdVideoAV1SegmentationFlags;

typedef struct StdVideoAV1Segmentation {
    StdVideoAV1SegmentationFlags flags;
    uint8_t                      feature_enabled[8][8];
    int16_t                      feature_value[8][8];
} StdVideoAV1Segmentation;

typedef struct StdVideoAV1LoopFilterFlags {
    uint8_t loop_filter_delta_enabled;
    uint8_t loop_filter_delta_update;
} StdVideoAV1LoopFilterFlags;

typedef struct StdVideoAV1LoopFilter {
    StdVideoAV1LoopFilterFlags flags;
    uint8_t loop_filter_level[4];
    uint8_t loop_filter_sharpness;
    uint8_t update_ref_delta[8];
    int8_t  loop_filter_ref_deltas[8];
    uint8_t update_mode_delta[2];
    int8_t  loop_filter_mode_deltas[2];
} StdVideoAV1LoopFilter;

typedef struct StdVideoAV1FrameHeaderFlags {
    uint8_t show_existing_frame : 1;
    uint8_t show_frame : 1;
    uint8_t showable_frame : 1;
    uint8_t error_resilient_mode : 1;
    uint8_t disable_cdf_update : 1;
    uint8_t use_superres : 1;
    uint8_t render_and_frame_size_different : 1;
    uint8_t allow_screen_content_tools : 1;
    uint8_t is_filter_switchable : 1;
    uint8_t force_integer_mv : 1;
    uint8_t frame_size_override_flag : 1;
    uint8_t buffer_removal_time_present_flag : 1;
    uint8_t allow_intrabc : 1;
    uint8_t frame_refs_short_signaling : 1;
    uint8_t allow_high_precision_mv : 1;
    uint8_t is_motion_mode_switchable : 1;
    uint8_t use_ref_frame_mvs : 1;
    uint8_t disable_frame_end_update_cdf : 1;
    uint8_t allow_warped_motion : 1;
    uint8_t reduced_tx_set : 1;
    uint8_t reference_select : 1;
    uint8_t skip_mode_present : 1;
    uint8_t delta_q_present : 1;
    uint8_t using_qmatrix : 1;
} StdVideoAV1FrameHeaderFlags;

typedef struct StdVideoAV1FrameHeader {
    StdVideoAV1FrameHeaderFlags flags;

    uint8_t  frame_to_show_map_idx;
    uint32_t frame_presentation_time;
    uint32_t display_frame_id;

    uint8_t frame_type;

    uint32_t current_frame_id;
    uint8_t  order_hint;

    uint32_t buffer_removal_time[32]; // per operating point

    uint8_t  primary_ref_frame;
    uint16_t frame_width_minus_1;
    uint16_t frame_height_minus_1;
    uint8_t  coded_denom;
    uint16_t render_width_minus_1;
    uint16_t render_height_minus_1;

    uint8_t found_ref[7];

    uint8_t refresh_frame_flags;
    uint8_t ref_order_hint[8];
    uint8_t last_frame_idx;
    uint8_t golden_frame_idx;
    int8_t  ref_frame_idx[7];
    uint32_t delta_frame_id_minus1[7];

    uint8_t interpolation_filter;
    uint8_t tx_mode;

    StdVideoAV1Tiling                     tiling;
    StdVideoAV1Quantization               quantization;
    StdVideoAV1Segmentation               segmentation;
    StdVideoAV1DeltaQ                     delta_q;
    StdVideoAV1LoopFilter                 loop_filter;
    StdVideoAV1CDEF                       cdef;
    StdVideoAV1LoopRestoration            lr;
    StdVideoAV1WarpedMotion               warped_motion[8]; // One per ref frame
    StdVideoAV1FilmGrainParameters        film_grain;
} StdVideoAV1FrameHeader;

typedef struct StdVideoAV1OperatingPoint {
    uint16_t operating_point_idc;
    uint8_t  seq_level_idx;
    uint8_t  seq_tier;
    uint8_t  decoder_model_present_for_this_op;
    uint32_t decoder_buffer_delay;
    uint32_t encoder_buffer_delay;
    uint8_t  low_delay_mode_flag;
    uint8_t  initial_display_delay_present_for_this_op;
    uint8_t  initial_display_delay_minus_1;
} StdVideoAV1OperatingPoint;

typedef struct StdVideoAV1ScreenCodingFlags {
    uint8_t seq_choose_integer_mv : 1;
    uint8_t seq_force_integer_mv : 1;
} StdVideoAV1ScreenCodingFlags;

typedef struct StdVideoAV1ScreenCoding {
    StdVideoAV1ScreenCodingFlags flags;
    uint8_t seq_choose_screen_content_tools;
    uint8_t seq_force_screen_content_tools;
} StdVideoAV1ScreenCoding;

typedef struct StdVideoAV1TimingInfoFlags {
    uint8_t equal_picture_interval;
} StdVideoAV1TimingInfoFlags;

typedef struct StdVideoAV1TimingInfo {
    StdVideoAV1TimingInfoFlags flags;
    uint32_t num_units_in_display_tick;
    uint32_t time_scale;
    uint32_t num_ticks_per_picture_minus_1;
} StdVideoAV1TimingInfo;

typedef struct StdVideoAV1DecoderModelInfo {
    uint8_t  buffer_delay_length_minus_1;
    uint32_t num_units_in_decoding_tick;
    uint8_t  buffer_removal_time_length_minus_1;
    uint8_t  frame_presentation_time_length_minus_1;
} StdVideoAV1DecoderModelInfo;

typedef struct StdVideoAV1ColorConfigFlags {
    uint8_t high_bitdepth : 1;
    uint8_t twelve_bit : 1;
    uint8_t mono_chrome : 1;
    uint8_t color_description_present_flag : 1;
    uint8_t color_range : 1;
    uint8_t separate_uv_delta_q : 1;
} StdVideoAV1ColorConfigFlags;

typedef struct StdVideoAV1ColorConfig {
    StdVideoAV1ColorConfigFlags flags;
    uint8_t color_primaries;
    uint8_t transfer_characteristics;
    uint8_t matrix_coefficients;
    uint8_t subsampling_x;
    uint8_t subsampling_y;
    uint8_t chroma_sample_position;
} StdVideoAV1ColorConfig;

typedef struct StdVideoAV1SequenceHeaderFlags {
    uint8_t still_picture : 1;
    uint8_t reduced_still_picture_header : 1;
    uint8_t use_128x128_superblock : 1;
    uint8_t enable_filter_intra : 1;
    uint8_t enable_intra_edge_filter : 1;
    uint8_t enable_interintra_compound : 1;
    uint8_t enable_masked_compound : 1;
    uint8_t enable_warped_motion : 1;
    uint8_t enable_dual_filter : 1;
    uint8_t enable_order_hint : 1;
    uint8_t enable_jnt_comp : 1;
    uint8_t enable_ref_frame_mvs : 1;
    uint8_t frame_id_numbers_present_flag : 1;
    uint8_t enable_superres : 1;
    uint8_t enable_cdef : 1;
    uint8_t enable_restoration : 1;
    uint8_t film_grain_params_present : 1;
    uint8_t timing_info_present_flag : 1;
    uint8_t decoder_model_info_present_flag : 1;
    uint8_t initial_display_delay_present_flag : 1;
} StdVideoAV1SequenceHeaderFlags;

typedef struct StdVideoAV1SequenceHeader {
    StdVideoAV1SequenceHeaderFlags flags;

    StdVideoAV1Profile seq_profile;
    uint8_t  operating_points_cnt_minus_1;
    uint8_t  frame_width_bits_minus_1;
    uint8_t  frame_height_bits_minus_1;
    uint16_t max_frame_width_minus_1;
    uint16_t max_frame_height_minus_1;
    uint8_t  delta_frame_id_length_minus_2;
    uint8_t  additional_frame_id_length_minus_1;
    uint8_t  order_hint_bits_minus_1;

    StdVideoAV1TimingInfo       timing_info;
    StdVideoAV1DecoderModelInfo decoder_model_info;
    StdVideoAV1OperatingPoint   operating_points[32];
    StdVideoAV1ColorConfig      color_config;
} StdVideoAV1SequenceHeader;

typedef struct StdVideoDecodeAV1ReferenceInfo {
    /* These are not necessary for decoding as per the spec, maybe should remove them */
    uint8_t temporal_id;
    uint8_t spatial_id;
    uint16_t display_frame_id;
} StdVideoDecodeAV1ReferenceInfo;

typedef struct StdVideoDecodeAV1Tile {
    uint16_t tg_start;
    uint16_t tg_end;
    uint16_t row;
    uint16_t column;
    int size;
    uint32_t offset;
} StdVideoDecodeAV1Tile;

typedef struct StdVideoDecodeAV1TileList {
    StdVideoDecodeAV1Tile *tile_list;
    uint32_t nb_tiles;
} StdVideoDecodeAV1TileList;

typedef struct VkVideoDecodeAV1PictureInfoMESA {
    VkStructureType sType;
    const void *pNext;
    StdVideoAV1FrameHeader *frame_header;
    StdVideoDecodeAV1TileList *tile_list;
} VkVideoDecodeAV1PictureInfoMESA;

typedef struct VkVideoDecodeAV1DpbSlotInfoMESA {
    VkStructureType sType;
    const void *pNext;
    const StdVideoDecodeAV1ReferenceInfo *pStdReferenceInfo;
} VkVideoDecodeAV1DpbSlotInfoMESA;

typedef struct VkVideoDecodeAV1SessionParametersAddInfoMESA {
    VkStructureType sType;
    const void *pNext;
    StdVideoAV1SequenceHeader *sequence_header;
} VkVideoDecodeAV1SessionParametersAddInfoMESA;

typedef struct VkVideoDecodeAV1SessionParametersCreateInfoMESA {
    VkStructureType sType;
    const void *pNext;
    const VkVideoDecodeAV1SessionParametersAddInfoMESA *pParametersAddInfo;
} VkVideoDecodeAV1SessionParametersCreateInfoMESA;

#define VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_PICTURE_INFO_MESA 100510001
#define VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_SESSION_PARAMETERS_CREATE_INFO_MESA 100510002
#define VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_SESSION_PARAMETERS_ADD_INFO_MESA 100510003
#define VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_DPB_SLOT_INFO_MESA 100510004


typedef struct VkVideoDecodeAV1ProfileInfoMESA {
    VkStructureType sType;
    const void *pNext;
    StdVideoAV1Profile stdProfileIdc;
} VkVideoDecodeAV1ProfileInfoMESA;

typedef struct VkVideoDecodeAV1CapabilitiesMESA {
    VkStructureType sType;
    const void *pNext;
    StdVideoAV1Level maxLevelIdc;
} VkVideoDecodeAV1CapabilitiesMESA;

#define VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_CAPABILITIES_MESA 100510005
#define VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_PROFILE_INFO_MESA 100510006

#ifdef __cplusplus
}
#endif

#endif
