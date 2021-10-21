#ifndef RADV_VIDEO_STRUCTS
#define RADV_VIDEO_STRUCTS

typedef struct rvcn_dec_message_index_s {
   unsigned int message_id;
   unsigned int offset;
   unsigned int size;
   unsigned int filled;
} rvcn_dec_message_index_t;

typedef struct rvcn_dec_message_header_s {
   unsigned int header_size;
   unsigned int total_size;
   unsigned int num_buffers;
   unsigned int msg_type;
   unsigned int stream_handle;
   unsigned int status_report_feedback_number;

   rvcn_dec_message_index_t index[1];
} rvcn_dec_message_header_t;

typedef struct rvcn_dec_message_create_s {
   unsigned int stream_type;
   unsigned int session_flags;
   unsigned int width_in_samples;
   unsigned int height_in_samples;
} rvcn_dec_message_create_t;

typedef struct rvcn_dec_message_decode_s {
   unsigned int stream_type;
   unsigned int decode_flags;
   unsigned int width_in_samples;
   unsigned int height_in_samples;

   unsigned int bsd_size;
   unsigned int dpb_size;
   unsigned int dt_size;
   unsigned int sct_size;
   unsigned int sc_coeff_size;
   unsigned int hw_ctxt_size;
   unsigned int sw_ctxt_size;
   unsigned int pic_param_size;
   unsigned int mb_cntl_size;
   unsigned int reserved0[4];
   unsigned int decode_buffer_flags;

   unsigned int db_pitch;
   unsigned int db_aligned_height;
   unsigned int db_tiling_mode;
   unsigned int db_swizzle_mode;
   unsigned int db_array_mode;
   unsigned int db_field_mode;
   unsigned int db_surf_tile_config;

   unsigned int dt_pitch;
   unsigned int dt_uv_pitch;
   unsigned int dt_tiling_mode;
   unsigned int dt_swizzle_mode;
   unsigned int dt_array_mode;
   unsigned int dt_field_mode;
   unsigned int dt_out_format;
   unsigned int dt_surf_tile_config;
   unsigned int dt_uv_surf_tile_config;
   unsigned int dt_luma_top_offset;
   unsigned int dt_luma_bottom_offset;
   unsigned int dt_chroma_top_offset;
   unsigned int dt_chroma_bottom_offset;
   unsigned int dt_chromaV_top_offset;
   unsigned int dt_chromaV_bottom_offset;

   unsigned int mif_wrc_en;
   unsigned int db_pitch_uv;

   unsigned char reserved1[20];
} rvcn_dec_message_decode_t;

typedef struct {
   unsigned short viewOrderIndex;
   unsigned short viewId;
   unsigned short numOfAnchorRefsInL0;
   unsigned short viewIdOfAnchorRefsInL0[15];
   unsigned short numOfAnchorRefsInL1;
   unsigned short viewIdOfAnchorRefsInL1[15];
   unsigned short numOfNonAnchorRefsInL0;
   unsigned short viewIdOfNonAnchorRefsInL0[15];
   unsigned short numOfNonAnchorRefsInL1;
   unsigned short viewIdOfNonAnchorRefsInL1[15];
} radeon_mvcElement_t;

typedef struct rvcn_dec_message_avc_s {
   unsigned int profile;
   unsigned int level;

   unsigned int sps_info_flags;
   unsigned int pps_info_flags;
   unsigned char chroma_format;
   unsigned char bit_depth_luma_minus8;
   unsigned char bit_depth_chroma_minus8;
   unsigned char log2_max_frame_num_minus4;

   unsigned char pic_order_cnt_type;
   unsigned char log2_max_pic_order_cnt_lsb_minus4;
   unsigned char num_ref_frames;
   unsigned char reserved_8bit;

   signed char pic_init_qp_minus26;
   signed char pic_init_qs_minus26;
   signed char chroma_qp_index_offset;
   signed char second_chroma_qp_index_offset;

   unsigned char num_slice_groups_minus1;
   unsigned char slice_group_map_type;
   unsigned char num_ref_idx_l0_active_minus1;
   unsigned char num_ref_idx_l1_active_minus1;

   unsigned short slice_group_change_rate_minus1;
   unsigned short reserved_16bit_1;

   unsigned char scaling_list_4x4[6][16];
   unsigned char scaling_list_8x8[2][64];

   unsigned int frame_num;
   unsigned int frame_num_list[16];
   int curr_field_order_cnt_list[2];
   int field_order_cnt_list[16][2];

   unsigned int decoded_pic_idx;
   unsigned int curr_pic_ref_frame_num;
   unsigned char ref_frame_list[16];

   unsigned int reserved[122];

   struct {
      unsigned int numViews;
      unsigned int viewId0;
      radeon_mvcElement_t mvcElements[1];
   } mvc;

} rvcn_dec_message_avc_t;

typedef struct rvcn_dec_feature_index_s {
   unsigned int feature_id;
   unsigned int offset;
   unsigned int size;
   unsigned int filled;
} rvcn_dec_feature_index_t;

typedef struct rvcn_dec_feedback_header_s {
   unsigned int header_size;
   unsigned int total_size;
   unsigned int num_buffers;
   unsigned int status_report_feedback_number;
   unsigned int status;
   unsigned int value;
   unsigned int errorBits;
   rvcn_dec_feature_index_t index[1];
} rvcn_dec_feedback_header_t;

#endif
