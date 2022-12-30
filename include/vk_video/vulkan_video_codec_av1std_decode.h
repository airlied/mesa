#ifndef VULKAN_VIDEO_CODEC_AV1STD_DECODE_H_
#define VULKAN_VIDEO_CODEC_AV1STD_DECODE_H_ 1


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
#define vulkan_video_codec_av1std_decode 1

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

   int8_t ar_coeffs_y[24];
   int8_t ar_coeffs_cb[25];
   int8_t ar_coeffs_cr[25];
   uint8_t cb_mult;
   uint8_t cb_luma_mult;
   uint16_t cb_offset;
   uint8_t cr_mult;
   uint8_t cr_luma_mult;
   uint16_t cr_offset;
} StdVideoAV1FilmGrainParameters;

typedef struct StdVideoDecodeAV1PictureInfo {
   StdVideoAV1Profile profile;
   StdVideoAV1Level level;

   StdVideoAV1PictureParameterSet picture_parameter;
} StdVideoDecodeAV1PictureInfo;

#ifdef __cplusplus
}
#endif

#endif
