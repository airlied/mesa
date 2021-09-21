#ifndef AC_VCN_DEC_REGS_H
#define AC_VCN_DEC_REGS_H

#define RDECODE_PKT_TYPE_S(x)        (((unsigned)(x)&0x3) << 30)
#define RDECODE_PKT_TYPE_G(x)        (((x) >> 30) & 0x3)
#define RDECODE_PKT_TYPE_C           0x3FFFFFFF
#define RDECODE_PKT_COUNT_S(x)       (((unsigned)(x)&0x3FFF) << 16)
#define RDECODE_PKT_COUNT_G(x)       (((x) >> 16) & 0x3FFF)
#define RDECODE_PKT_COUNT_C          0xC000FFFF
#define RDECODE_PKT0_BASE_INDEX_S(x) (((unsigned)(x)&0xFFFF) << 0)
#define RDECODE_PKT0_BASE_INDEX_G(x) (((x) >> 0) & 0xFFFF)
#define RDECODE_PKT0_BASE_INDEX_C    0xFFFF0000
#define RDECODE_PKT0(index, count)                                                                 \
   (RDECODE_PKT_TYPE_S(0) | RDECODE_PKT0_BASE_INDEX_S(index) | RDECODE_PKT_COUNT_S(count))

#define RDECODE_PKT2() (RDECODE_PKT_TYPE_S(2))

#define RDECODE_PKT_REG_J(x)  ((unsigned)(x)&0x3FFFF)
#define RDECODE_PKT_RES_J(x)  (((unsigned)(x)&0x3F) << 18)
#define RDECODE_PKT_COND_J(x) (((unsigned)(x)&0xF) << 24)
#define RDECODE_PKT_TYPE_J(x) (((unsigned)(x)&0xF) << 28)
#define RDECODE_PKTJ(reg, cond, type)                                                              \
   (RDECODE_PKT_REG_J(reg) | RDECODE_PKT_RES_J(0) | RDECODE_PKT_COND_J(cond) |                     \
    RDECODE_PKT_TYPE_J(type))

#define RDECODE_CMD_MSG_BUFFER                              0x00000000
#define RDECODE_CMD_DPB_BUFFER                              0x00000001
#define RDECODE_CMD_DECODING_TARGET_BUFFER                  0x00000002
#define RDECODE_CMD_FEEDBACK_BUFFER                         0x00000003
#define RDECODE_CMD_PROB_TBL_BUFFER                         0x00000004
#define RDECODE_CMD_SESSION_CONTEXT_BUFFER                  0x00000005
#define RDECODE_CMD_BITSTREAM_BUFFER                        0x00000100
#define RDECODE_CMD_IT_SCALING_TABLE_BUFFER                 0x00000204
#define RDECODE_CMD_CONTEXT_BUFFER                          0x00000206

#define RDECODE_MSG_CREATE                                  0x00000000
#define RDECODE_MSG_DECODE                                  0x00000001
#define RDECODE_MSG_DESTROY                                 0x00000002

#define RDECODE_CODEC_H264                                  0x00000000
#define RDECODE_CODEC_VC1                                   0x00000001
#define RDECODE_CODEC_MPEG2_VLD                             0x00000003
#define RDECODE_CODEC_MPEG4                                 0x00000004
#define RDECODE_CODEC_H264_PERF                             0x00000007
#define RDECODE_CODEC_JPEG                                  0x00000008
#define RDECODE_CODEC_H265                                  0x00000010
#define RDECODE_CODEC_VP9                                   0x00000011
#define RDECODE_CODEC_AV1                                   0x00000013

#define RDECODE_ARRAY_MODE_LINEAR                           0x00000000
#define RDECODE_ARRAY_MODE_MACRO_LINEAR_MICRO_TILED         0x00000001
#define RDECODE_ARRAY_MODE_1D_THIN                          0x00000002
#define RDECODE_ARRAY_MODE_2D_THIN                          0x00000004
#define RDECODE_ARRAY_MODE_MACRO_TILED_MICRO_LINEAR         0x00000004
#define RDECODE_ARRAY_MODE_MACRO_TILED_MICRO_TILED          0x00000005

#define RDECODE_H264_PROFILE_BASELINE                       0x00000000
#define RDECODE_H264_PROFILE_MAIN                           0x00000001
#define RDECODE_H264_PROFILE_HIGH                           0x00000002
#define RDECODE_H264_PROFILE_STEREO_HIGH                    0x00000003
#define RDECODE_H264_PROFILE_MVC                            0x00000004

#define RDECODE_VC1_PROFILE_SIMPLE                          0x00000000
#define RDECODE_VC1_PROFILE_MAIN                            0x00000001
#define RDECODE_VC1_PROFILE_ADVANCED                        0x00000002

#define RDECODE_SW_MODE_LINEAR                              0x00000000
#define RDECODE_256B_S                                      0x00000001
#define RDECODE_256B_D                                      0x00000002
#define RDECODE_4KB_S                                       0x00000005
#define RDECODE_4KB_D                                       0x00000006
#define RDECODE_64KB_S                                      0x00000009
#define RDECODE_64KB_D                                      0x0000000A
#define RDECODE_4KB_S_X                                     0x00000015
#define RDECODE_4KB_D_X                                     0x00000016
#define RDECODE_64KB_S_X                                    0x00000019
#define RDECODE_64KB_D_X                                    0x0000001A

#define RDECODE_MESSAGE_NOT_SUPPORTED                       0x00000000
#define RDECODE_MESSAGE_CREATE                              0x00000001
#define RDECODE_MESSAGE_DECODE                              0x00000002
#define RDECODE_MESSAGE_DRM                                 0x00000003
#define RDECODE_MESSAGE_AVC                                 0x00000006
#define RDECODE_MESSAGE_VC1                                 0x00000007
#define RDECODE_MESSAGE_MPEG2_VLD                           0x0000000A
#define RDECODE_MESSAGE_MPEG4_ASP_VLD                       0x0000000B
#define RDECODE_MESSAGE_HEVC                                0x0000000D
#define RDECODE_MESSAGE_VP9                                 0x0000000E
#define RDECODE_MESSAGE_DYNAMIC_DPB                         0x00000010
#define RDECODE_MESSAGE_AV1                                 0x00000011

#define RDECODE_FEEDBACK_PROFILING                          0x00000001

#define RDECODE_SPS_INFO_H264_EXTENSION_SUPPORT_FLAG_SHIFT  7

#define NUM_BUFFERS                                         4

#define RDECODE_VP9_PROBS_DATA_SIZE                         2304

#define mmUVD_JPEG_CNTL                                     0x0200
#define mmUVD_JPEG_CNTL_BASE_IDX                            1
#define mmUVD_JPEG_RB_BASE                                  0x0201
#define mmUVD_JPEG_RB_BASE_BASE_IDX                         1
#define mmUVD_JPEG_RB_WPTR                                  0x0202
#define mmUVD_JPEG_RB_WPTR_BASE_IDX                         1
#define mmUVD_JPEG_RB_RPTR                                  0x0203
#define mmUVD_JPEG_RB_RPTR_BASE_IDX                         1
#define mmUVD_JPEG_RB_SIZE                                  0x0204
#define mmUVD_JPEG_RB_SIZE_BASE_IDX                         1
#define mmUVD_JPEG_TIER_CNTL2                               0x021a
#define mmUVD_JPEG_TIER_CNTL2_BASE_IDX                      1
#define mmUVD_JPEG_UV_TILING_CTRL                           0x021c
#define mmUVD_JPEG_UV_TILING_CTRL_BASE_IDX                  1
#define mmUVD_JPEG_TILING_CTRL                              0x021e
#define mmUVD_JPEG_TILING_CTRL_BASE_IDX                     1
#define mmUVD_JPEG_OUTBUF_RPTR                              0x0220
#define mmUVD_JPEG_OUTBUF_RPTR_BASE_IDX                     1
#define mmUVD_JPEG_OUTBUF_WPTR                              0x0221
#define mmUVD_JPEG_OUTBUF_WPTR_BASE_IDX                     1
#define mmUVD_JPEG_PITCH                                    0x0222
#define mmUVD_JPEG_PITCH_BASE_IDX                           1
#define mmUVD_JPEG_INT_EN                                   0x0229
#define mmUVD_JPEG_INT_EN_BASE_IDX                          1
#define mmUVD_JPEG_UV_PITCH                                 0x022b
#define mmUVD_JPEG_UV_PITCH_BASE_IDX                        1
#define mmUVD_JPEG_INDEX                                    0x023e
#define mmUVD_JPEG_INDEX_BASE_IDX                           1
#define mmUVD_JPEG_DATA                                     0x023f
#define mmUVD_JPEG_DATA_BASE_IDX                            1
#define mmUVD_LMI_JPEG_WRITE_64BIT_BAR_HIGH                 0x0438
#define mmUVD_LMI_JPEG_WRITE_64BIT_BAR_HIGH_BASE_IDX        1
#define mmUVD_LMI_JPEG_WRITE_64BIT_BAR_LOW                  0x0439
#define mmUVD_LMI_JPEG_WRITE_64BIT_BAR_LOW_BASE_IDX         1
#define mmUVD_LMI_JPEG_READ_64BIT_BAR_HIGH                  0x045a
#define mmUVD_LMI_JPEG_READ_64BIT_BAR_HIGH_BASE_IDX         1
#define mmUVD_LMI_JPEG_READ_64BIT_BAR_LOW                   0x045b
#define mmUVD_LMI_JPEG_READ_64BIT_BAR_LOW_BASE_IDX          1
#define mmUVD_CTX_INDEX                                     0x0528
#define mmUVD_CTX_INDEX_BASE_IDX                            1
#define mmUVD_CTX_DATA                                      0x0529
#define mmUVD_CTX_DATA_BASE_IDX                             1
#define mmUVD_SOFT_RESET                                    0x05a0
#define mmUVD_SOFT_RESET_BASE_IDX                           1

#define vcnipUVD_JPEG_DEC_SOFT_RST                          0x402f
#define vcnipUVD_JRBC_IB_COND_RD_TIMER                      0x408e
#define vcnipUVD_JRBC_IB_REF_DATA                           0x408f
#define vcnipUVD_LMI_JPEG_READ_64BIT_BAR_HIGH               0x40e1
#define vcnipUVD_LMI_JPEG_READ_64BIT_BAR_LOW                0x40e0
#define vcnipUVD_JPEG_RB_BASE                               0x4001
#define vcnipUVD_JPEG_RB_SIZE                               0x4004
#define vcnipUVD_JPEG_RB_WPTR                               0x4002
#define vcnipUVD_JPEG_PITCH                                 0x401f
#define vcnipUVD_JPEG_UV_PITCH                              0x4020
#define vcnipJPEG_DEC_ADDR_MODE                             0x4027
#define vcnipJPEG_DEC_Y_GFX10_TILING_SURFACE                0x4024
#define vcnipJPEG_DEC_UV_GFX10_TILING_SURFACE               0x4025
#define vcnipUVD_LMI_JPEG_WRITE_64BIT_BAR_HIGH              0x40e3
#define vcnipUVD_LMI_JPEG_WRITE_64BIT_BAR_LOW               0x40e2
#define vcnipUVD_JPEG_INDEX                                 0x402c
#define vcnipUVD_JPEG_DATA                                  0x402d
#define vcnipUVD_JPEG_TIER_CNTL2                            0x400f
#define vcnipUVD_JPEG_OUTBUF_RPTR                           0x401e
#define vcnipUVD_JPEG_OUTBUF_CNTL                           0x401c
#define vcnipUVD_JPEG_INT_EN                                0x400a
#define vcnipUVD_JPEG_CNTL                                  0x4000
#define vcnipUVD_JPEG_RB_RPTR                               0x4003
#define vcnipUVD_JPEG_OUTBUF_WPTR                           0x401d

#define UVD_BASE_INST0_SEG0                                 0x00007800
#define UVD_BASE_INST0_SEG1                                 0x00007E00
#define UVD_BASE_INST0_SEG2                                 0
#define UVD_BASE_INST0_SEG3                                 0
#define UVD_BASE_INST0_SEG4                                 0

#define SOC15_REG_ADDR(reg) (UVD_BASE_INST0_SEG1 + reg)

#define COND0 0
#define COND1 1
#define COND2 2
#define COND3 3
#define COND4 4
#define COND5 5
#define COND6 6
#define COND7 7

#define TYPE0 0
#define TYPE1 1
#define TYPE2 2
#define TYPE3 3
#define TYPE4 4
#define TYPE5 5
#define TYPE6 6
#define TYPE7 7

/* VP9 Frame header flags */
#define RDECODE_FRAME_HDR_INFO_VP9_USE_UNCOMPRESSED_HEADER_SHIFT      (14)
#define RDECODE_FRAME_HDR_INFO_VP9_USE_PREV_IN_FIND_MV_REFS_SHIFT     (13)
#define RDECODE_FRAME_HDR_INFO_VP9_MODE_REF_DELTA_UPDATE_SHIFT        (12)
#define RDECODE_FRAME_HDR_INFO_VP9_MODE_REF_DELTA_ENABLED_SHIFT       (11)
#define RDECODE_FRAME_HDR_INFO_VP9_SEGMENTATION_UPDATE_DATA_SHIFT     (10)
#define RDECODE_FRAME_HDR_INFO_VP9_SEGMENTATION_TEMPORAL_UPDATE_SHIFT (9)
#define RDECODE_FRAME_HDR_INFO_VP9_SEGMENTATION_UPDATE_MAP_SHIFT      (8)
#define RDECODE_FRAME_HDR_INFO_VP9_SEGMENTATION_ENABLED_SHIFT         (7)
#define RDECODE_FRAME_HDR_INFO_VP9_FRAME_PARALLEL_DECODING_MODE_SHIFT (6)
#define RDECODE_FRAME_HDR_INFO_VP9_REFRESH_FRAME_CONTEXT_SHIFT        (5)
#define RDECODE_FRAME_HDR_INFO_VP9_ALLOW_HIGH_PRECISION_MV_SHIFT      (4)
#define RDECODE_FRAME_HDR_INFO_VP9_INTRA_ONLY_SHIFT                   (3)
#define RDECODE_FRAME_HDR_INFO_VP9_ERROR_RESILIENT_MODE_SHIFT         (2)
#define RDECODE_FRAME_HDR_INFO_VP9_FRAME_TYPE_SHIFT                   (1)
#define RDECODE_FRAME_HDR_INFO_VP9_SHOW_EXISTING_FRAME_SHIFT          (0)


#define RDECODE_FRAME_HDR_INFO_VP9_USE_UNCOMPRESSED_HEADER_MASK      (0x00004000)
#define RDECODE_FRAME_HDR_INFO_VP9_USE_PREV_IN_FIND_MV_REFS_MASK     (0x00002000)
#define RDECODE_FRAME_HDR_INFO_VP9_MODE_REF_DELTA_UPDATE_MASK        (0x00001000)
#define RDECODE_FRAME_HDR_INFO_VP9_MODE_REF_DELTA_ENABLED_MASK       (0x00000800)
#define RDECODE_FRAME_HDR_INFO_VP9_SEGMENTATION_UPDATE_DATA_MASK     (0x00000400)
#define RDECODE_FRAME_HDR_INFO_VP9_SEGMENTATION_TEMPORAL_UPDATE_MASK (0x00000200)
#define RDECODE_FRAME_HDR_INFO_VP9_SEGMENTATION_UPDATE_MAP_MASK      (0x00000100)
#define RDECODE_FRAME_HDR_INFO_VP9_SEGMENTATION_ENABLED_MASK         (0x00000080)
#define RDECODE_FRAME_HDR_INFO_VP9_FRAME_PARALLEL_DECODING_MODE_MASK (0x00000040)
#define RDECODE_FRAME_HDR_INFO_VP9_REFRESH_FRAME_CONTEXT_MASK        (0x00000020)
#define RDECODE_FRAME_HDR_INFO_VP9_ALLOW_HIGH_PRECISION_MV_MASK      (0x00000010)
#define RDECODE_FRAME_HDR_INFO_VP9_INTRA_ONLY_MASK                   (0x00000008)
#define RDECODE_FRAME_HDR_INFO_VP9_ERROR_RESILIENT_MODE_MASK         (0x00000004)
#define RDECODE_FRAME_HDR_INFO_VP9_FRAME_TYPE_MASK                   (0x00000002)
#define RDECODE_FRAME_HDR_INFO_VP9_SHOW_EXISTING_FRAME_MASK          (0x00000001)

/* Drm definitions */
#define DRM_CMD_KEY_SHIFT              0
#define DRM_CMD_CNT_KEY_SHIFT          1
#define DRM_CMD_CNT_DATA_SHIFT         2
#define DRM_CMD_OFFSET_SHIFT           3
#define DRM_CMD_SESSION_SEL_SHIFT      4
#define DRM_CMD_UNWRAP_KEY_SHIFT       8
#define DRM_CMD_GEN_MASK_SHIFT         9
#define DRM_CMD_ALGORITHM_SHIFT        10
#define DRM_CMD_BYTE_MASK_SHIFT        16
#define DRM_CMD_DRM_BYPASS_SHIFT       31

#define DRM_CMD_KEY_MASK               (0x00000001)
#define DRM_CMD_CNT_KEY_MASK           (0x00000002)
#define DRM_CMD_CNT_DATA_MASK          (0x00000004)
#define DRM_CMD_OFFSET_MASK            (0x00000008)
#define DRM_CMD_SESSION_SEL_MASK       (0x000000F0)
#define DRM_CMD_UNWRAP_KEY_MASK        (0x00000100)
#define DRM_CMD_GEN_MASK_MASK          (0x00000200)
#define DRM_CMD_ALGORITHM_MASK         (0x00000C00)
#define DRM_CMD_BYTE_MASK_MASK         (0x00FF0000)
#define DRM_CMD_DRM_BYPASS_MASK        (0x80000000)

/* Drm_cntl definitions */
#define DRM_CNTL_ENC_BYTECNT_SHIFT     (6)
#define DRM_CNTL_CLR_BYTECNT_SHIFT     (16)
#define DRM_CNTL_BYPASS_SHIFT          (24)
#define DRM_CNTL_PARTIAL_MODE_SHIFT    (25)
#define DRM_CNTL_OFFSET_MODE_SHIFT     (26)
#define DRM_CNTL_HEADER_MODE_SHIFT     (27)
#define DRM_CNTL_HEADER_BYTECNT_SHIFT  (28)

#define DRM_CNTL_ENC_BYTECNT_MASK      (0x00000FC0)
#define DRM_CNTL_CLR_BYTECNT_MASK      (0x003F0000)
#define DRM_CNTL_BYPASS_MASK           (0x01000000)
#define DRM_CNTL_PARTIAL_MODE_MASK     (0x02000000)
#define DRM_CNTL_OFFSET_MODE_MASK      (0x04000000)
#define DRM_CNTL_HEADER_MODE_MASK      (0x08000000)
#define DRM_CNTL_HEADER_BYTECNT_MASK   (0xF0000000)

#define SAMU_DRM_DISABLE 0x00000000
#define SAMU_DRM_ENABLE  0x00000001

/* AV1 Frame header flags */
#define RDECODE_FRAME_HDR_INFO_AV1_DISABLE_REF_FRAME_MVS_SHIFT        (31)
#define RDECODE_FRAME_HDR_INFO_AV1_SKIP_REFERENCE_UPDATE_SHIFT        (30)
#define RDECODE_FRAME_HDR_INFO_AV1_SWITCHABLE_SKIP_MODE_SHIFT         (29)
#define RDECODE_FRAME_HDR_INFO_AV1_DELTA_LF_MULTI_SHIFT               (28)
#define RDECODE_FRAME_HDR_INFO_AV1_SEGMENTATION_TEMPORAL_UPDATE_SHIFT (27)
#define RDECODE_FRAME_HDR_INFO_AV1_SEGMENTATION_UPDATE_MAP_SHIFT      (26)
#define RDECODE_FRAME_HDR_INFO_AV1_SEGMENTATION_ENABLED_SHIFT         (25)
#define RDECODE_FRAME_HDR_INFO_AV1_REDUCED_TX_SET_USED_SHIFT          (24)
#define RDECODE_FRAME_HDR_INFO_AV1_DELTA_LF_PRESENT_FLAG_SHIFT        (23)
#define RDECODE_FRAME_HDR_INFO_AV1_DELTA_Q_PRESENT_FLAG_SHIFT         (22)
#define RDECODE_FRAME_HDR_INFO_AV1_MODE_REF_DELTA_UPDATE_SHIFT        (21)
#define RDECODE_FRAME_HDR_INFO_AV1_MODE_REF_DELTA_ENABLED_SHIFT       (20)
#define RDECODE_FRAME_HDR_INFO_AV1_CUR_FRAME_FORCE_INTEGER_MV_SHIFT   (19)
#define RDECODE_FRAME_HDR_INFO_AV1_ALLOW_SCREEN_CONTENT_TOOLS_SHIFT   (18)
#define RDECODE_FRAME_HDR_INFO_AV1_ALLOW_REF_FRAME_MVS_SHIFT          (17)
#define RDECODE_FRAME_HDR_INFO_AV1_ENABLE_JNT_COMP_SHIFT              (16)
#define RDECODE_FRAME_HDR_INFO_AV1_ENABLE_ORDER_HINT_SHIFT            (15)
#define RDECODE_FRAME_HDR_INFO_AV1_ENABLE_DUAL_FILTER_SHIFT           (14)
#define RDECODE_FRAME_HDR_INFO_AV1_ALLOW_WARPED_MOTION_SHIFT          (13)
#define RDECODE_FRAME_HDR_INFO_AV1_ENABLE_MASKED_COMPOUND_SHIFT       (12)
#define RDECODE_FRAME_HDR_INFO_AV1_ENABLE_INTERINTRA_COMPOUND_SHIFT   (11)
#define RDECODE_FRAME_HDR_INFO_AV1_ENABLE_INTRA_EDGE_FILTER_SHIFT     (10)
#define RDECODE_FRAME_HDR_INFO_AV1_ENABLE_FILTER_INTRA_SHIFT          (9)
#define RDECODE_FRAME_HDR_INFO_AV1_USING_QMATRIX_SHIFT                (8)
#define RDECODE_FRAME_HDR_INFO_AV1_SKIP_MODE_FLAG_SHIFT               (7)
#define RDECODE_FRAME_HDR_INFO_AV1_MONOCHROME_SHIFT                   (6)
#define RDECODE_FRAME_HDR_INFO_AV1_ALLOW_HIGH_PRECISION_MV_SHIFT      (5)
#define RDECODE_FRAME_HDR_INFO_AV1_ALLOW_INTRABC_SHIFT                (4)
#define RDECODE_FRAME_HDR_INFO_AV1_INTRA_ONLY_SHIFT                   (3)
#define RDECODE_FRAME_HDR_INFO_AV1_REFRESH_FRAME_CONTEXT_SHIFT        (2)
#define RDECODE_FRAME_HDR_INFO_AV1_DISABLE_CDF_UPDATE_SHIFT           (1)
#define RDECODE_FRAME_HDR_INFO_AV1_SHOW_FRAME_SHIFT                   (0)

#define RDECODE_FRAME_HDR_INFO_AV1_DISABLE_REF_FRAME_MVS_MASK         (0x80000000)
#define RDECODE_FRAME_HDR_INFO_AV1_SKIP_REFERENCE_UPDATE_MASK         (0x40000000)
#define RDECODE_FRAME_HDR_INFO_AV1_SWITCHABLE_SKIP_MODE_MASK          (0x20000000)
#define RDECODE_FRAME_HDR_INFO_AV1_DELTA_LF_MULTI_MASK                (0x10000000)
#define RDECODE_FRAME_HDR_INFO_AV1_SEGMENTATION_TEMPORAL_UPDATE_MASK  (0x08000000)
#define RDECODE_FRAME_HDR_INFO_AV1_SEGMENTATION_UPDATE_MAP_MASK       (0x04000000)
#define RDECODE_FRAME_HDR_INFO_AV1_SEGMENTATION_ENABLED_MASK          (0x02000000)
#define RDECODE_FRAME_HDR_INFO_AV1_REDUCED_TX_SET_USED_MASK           (0x01000000)
#define RDECODE_FRAME_HDR_INFO_AV1_DELTA_LF_PRESENT_FLAG_MASK         (0x00800000)
#define RDECODE_FRAME_HDR_INFO_AV1_DELTA_Q_PRESENT_FLAG_MASK          (0x00400000)
#define RDECODE_FRAME_HDR_INFO_AV1_MODE_REF_DELTA_UPDATE_MASK         (0x00200000)
#define RDECODE_FRAME_HDR_INFO_AV1_MODE_REF_DELTA_ENABLED_MASK        (0x00100000)
#define RDECODE_FRAME_HDR_INFO_AV1_CUR_FRAME_FORCE_INTEGER_MV_MASK    (0x00080000)
#define RDECODE_FRAME_HDR_INFO_AV1_ALLOW_SCREEN_CONTENT_TOOLS_MASK    (0x00040000)
#define RDECODE_FRAME_HDR_INFO_AV1_ALLOW_REF_FRAME_MVS_MASK           (0x00020000)
#define RDECODE_FRAME_HDR_INFO_AV1_ENABLE_JNT_COMP_MASK               (0x00010000)
#define RDECODE_FRAME_HDR_INFO_AV1_ENABLE_ORDER_HINT_MASK             (0x00008000)
#define RDECODE_FRAME_HDR_INFO_AV1_ENABLE_DUAL_FILTER_MASK            (0x00004000)
#define RDECODE_FRAME_HDR_INFO_AV1_ALLOW_WARPED_MOTION_MASK           (0x00002000)
#define RDECODE_FRAME_HDR_INFO_AV1_ENABLE_MASKED_COMPOUND_MASK        (0x00001000)
#define RDECODE_FRAME_HDR_INFO_AV1_ENABLE_INTERINTRA_COMPOUND_MASK    (0x00000800)
#define RDECODE_FRAME_HDR_INFO_AV1_ENABLE_INTRA_EDGE_FILTER_MASK      (0x00000400)
#define RDECODE_FRAME_HDR_INFO_AV1_ENABLE_FILTER_INTRA_MASK           (0x00000200)
#define RDECODE_FRAME_HDR_INFO_AV1_USING_QMATRIX_MASK                 (0x00000100)
#define RDECODE_FRAME_HDR_INFO_AV1_SKIP_MODE_FLAG_MASK                (0x00000080)
#define RDECODE_FRAME_HDR_INFO_AV1_MONOCHROME_MASK                    (0x08000040)
#define RDECODE_FRAME_HDR_INFO_AV1_ALLOW_HIGH_PRECISION_MV_MASK       (0x00000020)
#define RDECODE_FRAME_HDR_INFO_AV1_ALLOW_INTRABC_MASK                 (0x00000010)
#define RDECODE_FRAME_HDR_INFO_AV1_INTRA_ONLY_MASK                    (0x00000008)
#define RDECODE_FRAME_HDR_INFO_AV1_REFRESH_FRAME_CONTEXT_MASK         (0x00000004)
#define RDECODE_FRAME_HDR_INFO_AV1_DISABLE_CDF_UPDATE_MASK            (0x00000002)
#define RDECODE_FRAME_HDR_INFO_AV1_SHOW_FRAME_MASK                    (0x00000001)

#define RDECODE_VCN1_GPCOM_VCPU_CMD   0x2070c
#define RDECODE_VCN1_GPCOM_VCPU_DATA0 0x20710
#define RDECODE_VCN1_GPCOM_VCPU_DATA1 0x20714
#define RDECODE_VCN1_ENGINE_CNTL      0x20718

#define RDECODE_VCN2_GPCOM_VCPU_CMD   (0x503 << 2)
#define RDECODE_VCN2_GPCOM_VCPU_DATA0 (0x504 << 2)
#define RDECODE_VCN2_GPCOM_VCPU_DATA1 (0x505 << 2)
#define RDECODE_VCN2_ENGINE_CNTL      (0x506 << 2)

#define RDECODE_VCN2_5_GPCOM_VCPU_CMD   0x3c
#define RDECODE_VCN2_5_GPCOM_VCPU_DATA0 0x40
#define RDECODE_VCN2_5_GPCOM_VCPU_DATA1 0x44
#define RDECODE_VCN2_5_ENGINE_CNTL      0x9b4

#endif
