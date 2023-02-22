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

#include "anv_private.h"

#include "genxml/gen_macros.h"
#include "genxml/genX_pack.h"

void
genX(CmdBeginVideoCodingKHR)(VkCommandBuffer commandBuffer,
                             const VkVideoBeginCodingInfoKHR *pBeginInfo)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_video_session, vid, pBeginInfo->videoSession);
   ANV_FROM_HANDLE(anv_video_session_params, params, pBeginInfo->videoSessionParameters);

   cmd_buffer->video.vid = vid;
   cmd_buffer->video.params = params;

   if (vid->vk.op != VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_MESA)
      return;

   if (!vid->cdf_initialized) {
      anv_init_av1_cdf_tables(cmd_buffer->device, vid);
      vid->cdf_initialized = true;
   }
}

void
genX(CmdControlVideoCodingKHR)(VkCommandBuffer commandBuffer,
                               const VkVideoCodingControlInfoKHR *pCodingControlInfo)
{

}

void
genX(CmdEndVideoCodingKHR)(VkCommandBuffer commandBuffer,
                           const VkVideoEndCodingInfoKHR *pEndCodingInfo)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);

   cmd_buffer->video.vid = NULL;
   cmd_buffer->video.params = NULL;
}

static void
anv_h264_decode_video(struct anv_cmd_buffer *cmd_buffer,
                      const VkVideoDecodeInfoKHR *frame_info)
{
   ANV_FROM_HANDLE(anv_buffer, src_buffer, frame_info->srcBuffer);
   struct anv_video_session *vid = cmd_buffer->video.vid;
   struct anv_video_session_params *params = cmd_buffer->video.params;
   const struct VkVideoDecodeH264PictureInfoKHR *h264_pic_info =
      vk_find_struct_const(frame_info->pNext, VIDEO_DECODE_H264_PICTURE_INFO_KHR);
   const StdVideoH264SequenceParameterSet *sps = vk_video_find_h264_dec_std_sps(&params->vk, h264_pic_info->pStdPictureInfo->seq_parameter_set_id);
   const StdVideoH264PictureParameterSet *pps = vk_video_find_h264_dec_std_pps(&params->vk, h264_pic_info->pStdPictureInfo->pic_parameter_set_id);

   anv_batch_emit(&cmd_buffer->batch, GENX(MI_FLUSH_DW), flush) {
      flush.DWordLength = 2;
      flush.VideoPipelineCacheInvalidate = 1;
   };

#if GFX_VER >= 12
   anv_batch_emit(&cmd_buffer->batch, GENX(MI_FORCE_WAKEUP), wake) {
      wake.MFXPowerWellControl = 1;
      wake.MaskBits = 768;
   }

   anv_batch_emit(&cmd_buffer->batch, GENX(MFX_WAIT), mfx) {
      mfx.MFXSyncControlFlag = 1;
   }
#endif

   anv_batch_emit(&cmd_buffer->batch, GENX(MFX_PIPE_MODE_SELECT), sel) {
      sel.StandardSelect = SS_AVC;
      sel.CodecSelect = Decode;
      sel.DecoderShortFormatMode = ShortFormatDriverInterface;
      sel.DecoderModeSelect = VLDMode; // Hardcoded

      sel.PreDeblockingOutputEnable = 0;
      sel.PostDeblockingOutputEnable = 1;
   }

#if GFX_VER >= 12
   anv_batch_emit(&cmd_buffer->batch, GENX(MFX_WAIT), mfx) {
      mfx.MFXSyncControlFlag = 1;
   }
#endif

   const struct anv_image_view *iv = anv_image_view_from_handle(frame_info->dstPictureResource.imageViewBinding);
   const struct anv_image *img = iv->image;
   anv_batch_emit(&cmd_buffer->batch, GENX(MFX_SURFACE_STATE), ss) {
      ss.Width = img->vk.extent.width - 1;
      ss.Height = img->vk.extent.height - 1;
      ss.SurfaceFormat = PLANAR_420_8; // assert on this?
      ss.InterleaveChroma = 1;
      ss.SurfacePitch = img->planes[0].primary_surface.isl.row_pitch_B - 1;
      ss.TiledSurface = img->planes[0].primary_surface.isl.tiling != ISL_TILING_LINEAR;
      ss.TileWalk = TW_YMAJOR;

      ss.YOffsetforUCb = align(img->vk.extent.height, 32);
      ss.YOffsetforVCr = align(img->vk.extent.height, 32);
   }

   anv_batch_emit(&cmd_buffer->batch, GENX(MFX_PIPE_BUF_ADDR_STATE), buf) {
      bool use_pre_deblock = false;
      if (use_pre_deblock) {
         buf.PreDeblockingDestinationAddress = anv_image_address(img,
                                                                 &img->planes[0].primary_surface.memory_range);
      } else {
         buf.PostDeblockingDestinationAddress = anv_image_address(img,
                                                                  &img->planes[0].primary_surface.memory_range);
      }
      buf.PreDeblockingDestinationAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, buf.PreDeblockingDestinationAddress.bo, 0),
      };
      buf.PostDeblockingDestinationAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, buf.PostDeblockingDestinationAddress.bo, 0),
      };

      buf.IntraRowStoreScratchBufferAddress = (struct anv_address) { vid->vid_mem[ANV_VID_MEM_H264_INTRA_ROW_STORE].mem->bo, vid->vid_mem[ANV_VID_MEM_H264_INTRA_ROW_STORE].offset };
      buf.IntraRowStoreScratchBufferAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, buf.IntraRowStoreScratchBufferAddress.bo, 0),
      };
      buf.DeblockingFilterRowStoreScratchAddress = (struct anv_address) { vid->vid_mem[ANV_VID_MEM_H264_DEBLOCK_FILTER_ROW_STORE].mem->bo, vid->vid_mem[ANV_VID_MEM_H264_DEBLOCK_FILTER_ROW_STORE].offset };
      buf.DeblockingFilterRowStoreScratchAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, buf.DeblockingFilterRowStoreScratchAddress.bo, 0),
      };
      buf.MBStatusBufferAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, NULL, 0),
      };
      buf.MBILDBStreamOutBufferAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, NULL, 0),
      };
      buf.SecondMBILDBStreamOutBufferAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, NULL, 0),
      };
      buf.ScaledReferenceSurfaceAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, NULL, 0),
      };
      buf.OriginalUncompressedPictureSourceAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, NULL, 0),
      };
      buf.StreamOutDataDestinationAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, NULL, 0),
      };

      struct anv_bo *ref_bo = NULL;
      for (unsigned i = 0; i < frame_info->referenceSlotCount; i++) {
         const struct anv_image_view *ref_iv = anv_image_view_from_handle(frame_info->pReferenceSlots[i].pPictureResource->imageViewBinding);
         int idx = frame_info->pReferenceSlots[i].slotIndex;
         buf.ReferencePictureAddress[idx] = anv_image_address(ref_iv->image,
                                                              &ref_iv->image->planes[0].primary_surface.memory_range);

         if (i == 0) {
            ref_bo = ref_iv->image->bindings[0].address.bo;
         }
      }
      buf.ReferencePictureAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, ref_bo, 0),
      };
   }

   anv_batch_emit(&cmd_buffer->batch, GENX(MFX_IND_OBJ_BASE_ADDR_STATE), index_obj) {
      index_obj.MFXIndirectBitstreamObjectAddress = anv_address_add(src_buffer->address,
                                                                    frame_info->srcBufferOffset & ~4095);
      index_obj.MFXIndirectBitstreamObjectAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, src_buffer->address.bo, 0),
      };
      index_obj.MFXIndirectMVObjectAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, NULL, 0),
      };
      index_obj.MFDIndirectITCOEFFObjectAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, NULL, 0),
      };
      index_obj.MFDIndirectITDBLKObjectAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, NULL, 0),
      };
      index_obj.MFCIndirectPAKBSEObjectAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, NULL, 0),
      };
   }

   anv_batch_emit(&cmd_buffer->batch, GENX(MFX_BSP_BUF_BASE_ADDR_STATE), bsp) {
      bsp.BSDMPCRowStoreScratchBufferAddress = (struct anv_address) { vid->vid_mem[ANV_VID_MEM_H264_BSD_MPC_ROW_SCRATCH].mem->bo,
         vid->vid_mem[ANV_VID_MEM_H264_BSD_MPC_ROW_SCRATCH].offset };

      bsp.BSDMPCRowStoreScratchBufferAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, bsp.BSDMPCRowStoreScratchBufferAddress.bo, 0),
      };
      bsp.MPRRowStoreScratchBufferAddress = (struct anv_address) { vid->vid_mem[ANV_VID_MEM_H264_MPR_ROW_SCRATCH].mem->bo,
         vid->vid_mem[ANV_VID_MEM_H264_BSD_MPC_ROW_SCRATCH].offset };

      bsp.MPRRowStoreScratchBufferAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, bsp.MPRRowStoreScratchBufferAddress.bo, 0),
      };
      bsp.BitplaneReadBufferAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, NULL, 0),
      };
   }

   anv_batch_emit(&cmd_buffer->batch, GENX(MFD_AVC_DPB_STATE), avc_dpb) {
      for (unsigned i = 0; i < frame_info->referenceSlotCount; i++) {
         const struct VkVideoDecodeH264DpbSlotInfoKHR *dpb_slot =
            vk_find_struct_const(frame_info->pReferenceSlots[i].pNext, VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR);
         const StdVideoDecodeH264ReferenceInfo *ref_info = dpb_slot->pStdReferenceInfo;
         int idx = frame_info->pReferenceSlots[i].slotIndex;
         avc_dpb.NonExistingFrame[idx] = ref_info->flags.is_non_existing;
         avc_dpb.LongTermFrame[idx] = ref_info->flags.used_for_long_term_reference;
         if (!ref_info->flags.top_field_flag && !ref_info->flags.bottom_field_flag)
            avc_dpb.UsedforReference[idx] = 3;
         else
            avc_dpb.UsedforReference[idx] = ref_info->flags.top_field_flag | (ref_info->flags.bottom_field_flag << 1);
         avc_dpb.LTSTFrameNumberList[idx] = ref_info->FrameNum;
      }
   }

   anv_batch_emit(&cmd_buffer->batch, GENX(MFD_AVC_PICID_STATE), picid) {
      picid.PictureIDRemappingDisable = true;
   }

   uint32_t pic_height = sps->pic_height_in_map_units_minus1 + 1;
   if (!sps->flags.frame_mbs_only_flag)
      pic_height *= 2;
   anv_batch_emit(&cmd_buffer->batch, GENX(MFX_AVC_IMG_STATE), avc_img) {
      avc_img.FrameWidth = sps->pic_width_in_mbs_minus1;
      avc_img.FrameHeight = pic_height - 1;
      avc_img.FrameSize = (sps->pic_width_in_mbs_minus1 + 1) * pic_height;

      if (!h264_pic_info->pStdPictureInfo->flags.field_pic_flag)
         avc_img.ImageStructure = FramePicture;
      else if (h264_pic_info->pStdPictureInfo->flags.bottom_field_flag)
         avc_img.ImageStructure = BottomFieldPicture;
      else
         avc_img.ImageStructure = TopFieldPicture;

      avc_img.WeightedBiPredictionIDC = pps->weighted_bipred_idc;
      avc_img.WeightedPredictionEnable = pps->flags.weighted_pred_flag;
      avc_img.FirstChromaQPOffset = pps->chroma_qp_index_offset;
      avc_img.SecondChromaQPOffset = pps->second_chroma_qp_index_offset;
      avc_img.FieldPicture = h264_pic_info->pStdPictureInfo->flags.field_pic_flag;
      avc_img.MBAFFMode = (sps->flags.mb_adaptive_frame_field_flag &&
                           !h264_pic_info->pStdPictureInfo->flags.field_pic_flag);
      avc_img.FrameMBOnly = sps->flags.frame_mbs_only_flag;
      avc_img._8x8IDCTTransformMode = pps->flags.transform_8x8_mode_flag;
      avc_img.Direct8x8Inference = sps->flags.direct_8x8_inference_flag;
      avc_img.ConstrainedIntraPrediction = pps->flags.constrained_intra_pred_flag;
      avc_img.NonReferencePicture = !h264_pic_info->pStdPictureInfo->flags.is_reference;
      avc_img.EntropyCodingSyncEnable = pps->flags.entropy_coding_mode_flag;
      avc_img.ChromaFormatIDC = sps->chroma_format_idc;
      avc_img.TrellisQuantizationChromaDisable = true;
      avc_img.NumberofReferenceFrames = frame_info->referenceSlotCount;
      avc_img.NumberofActiveReferencePicturesfromL0 = pps->num_ref_idx_l0_default_active_minus1 + 1;
      avc_img.NumberofActiveReferencePicturesfromL1 = pps->num_ref_idx_l1_default_active_minus1 + 1;
      avc_img.InitialQPValue = pps->pic_init_qp_minus26;
      avc_img.PicOrderPresent = pps->flags.bottom_field_pic_order_in_frame_present_flag;
      avc_img.DeltaPicOrderAlwaysZero = sps->flags.delta_pic_order_always_zero_flag;
      avc_img.PicOrderCountType = sps->pic_order_cnt_type;
      avc_img.DeblockingFilterControlPresent = pps->flags.deblocking_filter_control_present_flag;
      avc_img.RedundantPicCountPresent = pps->flags.redundant_pic_cnt_present_flag;
      avc_img.Log2MaxFrameNumber = sps->log2_max_frame_num_minus4;
      avc_img.Log2MaxPicOrderCountLSB = sps->log2_max_pic_order_cnt_lsb_minus4;
      avc_img.CurrentPictureFrameNumber = h264_pic_info->pStdPictureInfo->frame_num;
   }

   if (pps->flags.pic_scaling_matrix_present_flag) {
      anv_batch_emit(&cmd_buffer->batch, GENX(MFX_QM_STATE), qm) {
         qm.DWordLength = 16;
         qm.AVC = AVC_4x4_Intra_MATRIX;
         for (unsigned m = 0; m < 3; m++)
            for (unsigned q = 0; q < 16; q++)
               qm.ForwardQuantizerMatrix[m * 16 + q] = pps->pScalingLists->ScalingList4x4[m][q];
      }
      anv_batch_emit(&cmd_buffer->batch, GENX(MFX_QM_STATE), qm) {
         qm.DWordLength = 16;
         qm.AVC = AVC_4x4_Inter_MATRIX;
         for (unsigned m = 0; m < 3; m++)
            for (unsigned q = 0; q < 16; q++)
               qm.ForwardQuantizerMatrix[m * 16 + q] = pps->pScalingLists->ScalingList4x4[m + 3][q];
      }
      if (pps->flags.transform_8x8_mode_flag) {
         anv_batch_emit(&cmd_buffer->batch, GENX(MFX_QM_STATE), qm) {
            qm.DWordLength = 16;
            qm.AVC = AVC_8x8_Intra_MATRIX;
            for (unsigned q = 0; q < 64; q++)
               qm.ForwardQuantizerMatrix[q] = pps->pScalingLists->ScalingList8x8[0][q];
         }
         anv_batch_emit(&cmd_buffer->batch, GENX(MFX_QM_STATE), qm) {
            qm.DWordLength = 16;
            qm.AVC = AVC_8x8_Inter_MATRIX;
            for (unsigned q = 0; q < 64; q++)
               qm.ForwardQuantizerMatrix[q] = pps->pScalingLists->ScalingList8x8[3][q];
         }
      }
   } else if (sps->flags.seq_scaling_matrix_present_flag) {
      anv_batch_emit(&cmd_buffer->batch, GENX(MFX_QM_STATE), qm) {
         qm.DWordLength = 16;
         qm.AVC = AVC_4x4_Intra_MATRIX;
         for (unsigned m = 0; m < 3; m++)
            for (unsigned q = 0; q < 16; q++)
               qm.ForwardQuantizerMatrix[m * 16 + q] = sps->pScalingLists->ScalingList4x4[m][q];
      }
      anv_batch_emit(&cmd_buffer->batch, GENX(MFX_QM_STATE), qm) {
         qm.DWordLength = 16;
         qm.AVC = AVC_4x4_Inter_MATRIX;
         for (unsigned m = 0; m < 3; m++)
            for (unsigned q = 0; q < 16; q++)
               qm.ForwardQuantizerMatrix[m * 16 + q] = sps->pScalingLists->ScalingList4x4[m + 3][q];
      }
      if (pps->flags.transform_8x8_mode_flag) {
         anv_batch_emit(&cmd_buffer->batch, GENX(MFX_QM_STATE), qm) {
            qm.DWordLength = 16;
            qm.AVC = AVC_8x8_Intra_MATRIX;
            for (unsigned q = 0; q < 64; q++)
               qm.ForwardQuantizerMatrix[q] = sps->pScalingLists->ScalingList8x8[0][q];
         }
         anv_batch_emit(&cmd_buffer->batch, GENX(MFX_QM_STATE), qm) {
            qm.DWordLength = 16;
            qm.AVC = AVC_8x8_Inter_MATRIX;
            for (unsigned q = 0; q < 64; q++)
               qm.ForwardQuantizerMatrix[q] = sps->pScalingLists->ScalingList8x8[3][q];
         }
      }
   } else {
      anv_batch_emit(&cmd_buffer->batch, GENX(MFX_QM_STATE), qm) {
         qm.DWordLength = 16;
         qm.AVC = AVC_4x4_Intra_MATRIX;
         for (unsigned q = 0; q < 3 * 16; q++)
            qm.ForwardQuantizerMatrix[q] = 0x10;
      }
      anv_batch_emit(&cmd_buffer->batch, GENX(MFX_QM_STATE), qm) {
         qm.DWordLength = 16;
         qm.AVC = AVC_4x4_Inter_MATRIX;
         for (unsigned q = 0; q < 3 * 16; q++)
            qm.ForwardQuantizerMatrix[q] = 0x10;
      }
      if (pps->flags.transform_8x8_mode_flag) {
         anv_batch_emit(&cmd_buffer->batch, GENX(MFX_QM_STATE), qm) {
            qm.DWordLength = 16;
            qm.AVC = AVC_8x8_Intra_MATRIX;
            for (unsigned q = 0; q < 64; q++)
               qm.ForwardQuantizerMatrix[q] = 0x10;
         }
         anv_batch_emit(&cmd_buffer->batch, GENX(MFX_QM_STATE), qm) {
            qm.DWordLength = 16;
            qm.AVC = AVC_8x8_Inter_MATRIX;
            for (unsigned q = 0; q < 64; q++)
               qm.ForwardQuantizerMatrix[q] = 0x10;
         }
      }
   }

   anv_batch_emit(&cmd_buffer->batch, GENX(MFX_AVC_DIRECTMODE_STATE), avc_directmode) {
      /* bind reference frame DMV */
      struct anv_bo *dmv_bo = NULL;
      for (unsigned i = 0; i < frame_info->referenceSlotCount; i++) {
         int idx = frame_info->pReferenceSlots[i].slotIndex;
         const struct VkVideoDecodeH264DpbSlotInfoKHR *dpb_slot =
            vk_find_struct_const(frame_info->pReferenceSlots[i].pNext, VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR);
         const struct anv_image_view *ref_iv = anv_image_view_from_handle(frame_info->pReferenceSlots[i].pPictureResource->imageViewBinding);
         const StdVideoDecodeH264ReferenceInfo *ref_info = dpb_slot->pStdReferenceInfo;
         avc_directmode.DirectMVBufferAddress[idx] = anv_image_address(ref_iv->image,
                                                                     &ref_iv->image->vid_dmv_top_surface);
         if (i == 0) {
            dmv_bo = ref_iv->image->bindings[0].address.bo;
         }
         avc_directmode.POCList[2 * idx] = ref_info->PicOrderCnt[0];
         avc_directmode.POCList[2 * idx + 1] = ref_info->PicOrderCnt[1];
      }
      avc_directmode.DirectMVBufferAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, dmv_bo, 0),
      };

      avc_directmode.DirectMVBufferWriteAddress = anv_image_address(img,
                                                                    &img->vid_dmv_top_surface);
      avc_directmode.DirectMVBufferWriteAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, img->bindings[0].address.bo, 0),
      };
      avc_directmode.POCList[32] = h264_pic_info->pStdPictureInfo->PicOrderCnt[0];
      avc_directmode.POCList[33] = h264_pic_info->pStdPictureInfo->PicOrderCnt[1];
   }

   uint32_t buffer_offset = frame_info->srcBufferOffset & 4095;
#define HEADER_OFFSET 3
   for (unsigned s = 0; s < h264_pic_info->sliceCount; s++) {
      bool last_slice = s == (h264_pic_info->sliceCount - 1);
      uint32_t current_offset = h264_pic_info->pSliceOffsets[s];
      uint32_t this_end;
      if (!last_slice) {
         uint32_t next_offset = h264_pic_info->pSliceOffsets[s + 1];
         uint32_t next_end = h264_pic_info->pSliceOffsets[s + 2];
         if (s == h264_pic_info->sliceCount - 2)
            next_end = frame_info->srcBufferRange;
         anv_batch_emit(&cmd_buffer->batch, GENX(MFD_AVC_SLICEADDR), sliceaddr) {
            sliceaddr.IndirectBSDDataLength = next_end - next_offset - HEADER_OFFSET;
            /* start decoding after the 3-byte header. */
            sliceaddr.IndirectBSDDataStartAddress = buffer_offset + next_offset + HEADER_OFFSET;
         };
         this_end = next_offset;
      } else
         this_end = frame_info->srcBufferRange;
      anv_batch_emit(&cmd_buffer->batch, GENX(MFD_AVC_BSD_OBJECT), avc_bsd) {
         avc_bsd.IndirectBSDDataLength = this_end - current_offset - HEADER_OFFSET;
         /* start decoding after the 3-byte header. */
         avc_bsd.IndirectBSDDataStartAddress = buffer_offset + current_offset + HEADER_OFFSET;
         avc_bsd.InlineData.LastSlice = last_slice;
         avc_bsd.InlineData.FixPrevMBSkipped = 1;
         avc_bsd.InlineData.IntraPredictionErrorControl = 1;
         avc_bsd.InlineData.Intra8x84x4PredictionErrorConcealmentControl = 1;
         avc_bsd.InlineData.ISliceConcealmentMode = 1;
      };
   }
}

#if GFX_VERx10 >= 120

enum av1_seg_index
{
   SEG_LVL_ALT_Q          = 0,            //!< Use alternate Quantizer
   SEG_LVL_ALT_LFYV,                      //!< Use alternate loop filter value on y plane vertical
   SEG_LVL_ALT_LFYH,                      //!< Use alternate loop filter value on y plane horizontal
   SEG_LVL_ALT_LFU,                       //!< Use alternate loop filter value on u plane
   SEG_LVL_ALT_LFV,                       //!< Use alternate loop filter value on v plane
   SEG_LVL_REF_FRAME,                     //!< Optional Segment reference frame
   SEG_LVL_SKIP,                          //!< Optional Segment (0,0) + skip mode
   SEG_LVL_GLOBAL_MV,                     //!< Global MV
};

enum av1_ref_frame
{
   AV1_NONE_FRAME               = -1,       //!< none frame
   AV1_INTRA_FRAME              = 0,        //!< intra frame, which means the current frame
   AV1_LAST_FRAME               = 1,        //!< last frame
   AV1_LAST2_FRAME              = 2,        //!< last2 frame
   AV1_LAST3_FRAME              = 3,        //!< last3 frame
   AV1_GOLDEN_FRAME             = 4,        //!< golden frame
   AV1_BWDREF_FRAME             = 5,        //!< bwdref frame
   AV1_ALTREF2_FRAME            = 6,        //!< altref2 frame
   AV1_ALTREF_FRAME             = 7,        //!< altref frame
   AV1_TOTAL_REFS_PER_FRAME     = 8,        //!< total reference frame number
   AV1_NUM_INTER_REFS           = AV1_ALTREF_FRAME - AV1_LAST_FRAME + 1   //!< total number of inter ref frames
};

enum av1_frame_type
{
   AV1_KEY_FRAME        = 0,
   AV1_INTER_FRAME      = 1,
   AV1_INTRA_ONLY_FRAME = 2,  // replaces intra-only
   AV1_SFRAME           = 3,
   AV1_FRAME_TYPES,
};

static const uint32_t btdl_cache_offset = 0;
static const uint32_t smvl_cache_offset = 128;
static const uint32_t ipdl_cache_offset = 384;
static const uint32_t dfly_cache_offset = 640;
static const uint32_t dflu_cache_offset = 1344;
static const uint32_t dflv_cache_offset = 1536;
static const uint32_t cdef_cache_offset = 1728;

static const uint32_t av1_max_qindex          = 255;
static const uint32_t av1_num_qm_levels       = 16;
static const uint32_t av1_scaling_factor      = (1 << 14);

static uint32_t get_qindex(const VkVideoDecodeAV1PictureInfoMESA *av1_pic_info,
                           uint32_t segment_id)
{
   uint8_t base_qindex = av1_pic_info->frame_header->quantization.base_q_idx;
   uint32_t feature_mask = av1_pic_info->frame_header->segmentation.feature_enabled_bits[segment_id];
   if (av1_pic_info->frame_header->segmentation.flags.segmentation_enabled &&
       feature_mask & (1 << SEG_LVL_ALT_Q)) {
      int data = av1_pic_info->frame_header->segmentation.feature_data[segment_id][SEG_LVL_ALT_Q];
      return CLAMP(base_qindex + data, 0, av1_max_qindex);
   } else
      return base_qindex;
}

static bool frame_is_key_or_intra(const VkVideoDecodeAV1PictureInfoMESA *av1_pic_info)
{
   return (av1_pic_info->frame_header->frame_type == AV1_INTRA_ONLY_FRAME ||
           av1_pic_info->frame_header->frame_type == AV1_KEY_FRAME);
}

static int32_t get_relative_dist(const VkVideoDecodeAV1PictureInfoMESA *av1_pic_info,
                                 const struct anv_video_session_params *params,
                                 int32_t a, int32_t b)
{
   if (!params->vk.av1_dec.seq_hdr.flags.enable_order_hint)
      return 0;

   int32_t bits = params->vk.av1_dec.seq_hdr.order_hint_bits_minus_1 + 1;
   int32_t diff = a - b;
   int32_t m = 1 << (bits - 1);
   diff = (diff & (m - 1)) - (diff & m);
   return diff;
}

static void
anv_av1_decode_video_tile(struct anv_cmd_buffer *cmd_buffer,
                          const VkVideoDecodeInfoKHR *frame_info,
                          int tile_idx)
{
   ANV_FROM_HANDLE(anv_buffer, src_buffer, frame_info->srcBuffer);
   struct anv_video_session *vid = cmd_buffer->video.vid;
   struct anv_video_session_params *params = cmd_buffer->video.params;
   const struct VkVideoDecodeAV1PictureInfoMESA *av1_pic_info =
      vk_find_struct_const(frame_info->pNext, VIDEO_DECODE_AV1_PICTURE_INFO_MESA);

   int cdf_index = 0;
   if (av1_pic_info->frame_header->quantization.base_q_idx <= 20)
      cdf_index = 0;
   else if (av1_pic_info->frame_header->quantization.base_q_idx <= 60)
      cdf_index = 1;
   else if (av1_pic_info->frame_header->quantization.base_q_idx <= 120)
      cdf_index = 2;
   else
      cdf_index = 3;


   anv_batch_emit(&cmd_buffer->batch, GENX(MI_FORCE_WAKEUP), wake) {
      wake.MaskBits = 768;
   }
   anv_batch_emit(&cmd_buffer->batch, GENX(MI_FORCE_WAKEUP), wake) {
      wake.MaskBits = 768;
   }

   anv_batch_emit(&cmd_buffer->batch, GENX(MI_FLUSH_DW), flush) {
      flush.DWordLength = 2;
      flush.VideoPipelineCacheInvalidate = 1;
   };
   // VD_CONTROL
   anv_batch_emit(&cmd_buffer->batch, GENX(AVP_VD_CONTROL_STATE), vd) {
      vd.VDControlState.PipelineInitialization = 1;
   }

   anv_batch_emit(&cmd_buffer->batch, GENX(MFX_WAIT), mfx) {
      mfx.MFXSyncControlFlag = 1;
   }

   anv_batch_emit(&cmd_buffer->batch, GENX(AVP_PIPE_MODE_SELECT), sel) {
   };

   anv_batch_emit(&cmd_buffer->batch, GENX(MFX_WAIT), mfx) {
      mfx.MFXSyncControlFlag = 1;
   }

   struct refs_info {
      const struct anv_image *img;
      uint8_t order_hint;
      uint8_t ref_order_hints[7];
      uint8_t cdf_update_disabled;
   } ref_info[AV1_TOTAL_REFS_PER_FRAME] = {};

   const struct anv_image_view *dpb_iv = anv_image_view_from_handle(frame_info->pSetupReferenceSlot->pPictureResource->imageViewBinding);
   const struct anv_image *dpb_img = dpb_iv->image;

   ref_info[AV1_INTRA_FRAME].img = dpb_img;
   if (dpb_img && frame_info->referenceSlotCount) {
      ref_info[AV1_INTRA_FRAME].order_hint = av1_pic_info->frame_header->order_hint;
      ref_info[AV1_INTRA_FRAME].cdf_update_disabled = av1_pic_info->frame_header->flags.disable_frame_end_update_cdf;
   }

   for (enum av1_ref_frame r = AV1_LAST_FRAME; r <= AV1_ALTREF_FRAME; r++) {
      int ref_pic_idx = av1_pic_info->frame_header->ref_frame_idx[r - AV1_LAST_FRAME];
      ref_info[r].order_hint = av1_pic_info->frame_header->ref_order_hint[ref_pic_idx];

      for (unsigned i = 0; i < frame_info->referenceSlotCount; i++) {
         int idx = frame_info->pReferenceSlots[i].slotIndex;
         if (ref_pic_idx == idx) {
            const struct anv_image_view *ref_iv = anv_image_view_from_handle(frame_info->pReferenceSlots[i].pPictureResource->imageViewBinding);
            const struct anv_image *ref_img = ref_iv->image;
            const struct VkVideoDecodeAV1DpbSlotInfoMESA *dpb_slot =
               vk_find_struct_const(frame_info->pReferenceSlots[i].pNext, VIDEO_DECODE_AV1_DPB_SLOT_INFO_MESA);

            ref_info[r].img = ref_img;
            memcpy(ref_info[r].ref_order_hints, dpb_slot->ref_order_hints, 7);
            ref_info[r].cdf_update_disabled = dpb_slot->cdf_update_disabled;
         }
      }
   }

   const struct anv_image_view *iv = anv_image_view_from_handle(frame_info->dstPictureResource.imageViewBinding);
   const struct anv_image *img = iv->image;
   anv_batch_emit(&cmd_buffer->batch, GENX(AVP_SURFACE_STATE), ss) {
      ss.SurfaceFormat = AVP_PLANAR_420_8; // assert on this?
      ss.SurfacePitchMinus1 = img->planes[0].primary_surface.isl.row_pitch_B - 1;
      ss.YOffsetforUCb = img->planes[1].primary_surface.memory_range.offset / img->planes[0].primary_surface.isl.row_pitch_B;
   };


   for (enum av1_ref_frame r = AV1_INTRA_FRAME; r <= AV1_ALTREF_FRAME; r++) {
      if (ref_info[r].img && frame_info->referenceSlotCount) {
         anv_batch_emit(&cmd_buffer->batch, GENX(AVP_SURFACE_STATE), ss) {
            ss.SurfaceID = 0x6 + r;
            ss.SurfaceFormat = AVP_PLANAR_420_8;
            ss.SurfacePitchMinus1 = ref_info[r].img->planes[0].primary_surface.isl.row_pitch_B - 1;
            ss.YOffsetforUCb = ref_info[r].img->planes[1].primary_surface.memory_range.offset / ref_info[r].img->planes[0].primary_surface.isl.row_pitch_B;
         }
      }
   }

   bool use_internal_cache_mem = true;

#if GFX_VERx10 == 125
   assert(img->planes[0].primary_surface.isl.tiling == ISL_TILING_4);
#endif
   anv_batch_emit(&cmd_buffer->batch, GENX(AVP_PIPE_BUF_ADDR_STATE), buf) {
      buf.DecodedOutputFrameBufferAddress = anv_image_address(img,
                                                              &img->planes[0].primary_surface.memory_range);
      buf.DecodedOutputFrameBufferAddressAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, buf.DecodedOutputFrameBufferAddress.bo, 0),
#if GFX_VERx10 >= 125
         .TiledResourceMode = TRMODE_TILEF,
#endif
      };
      buf.CurrentFrameMVWriteBufferAddress = anv_image_address(dpb_img,
                                                               &dpb_img->vid_dmv_top_surface);
      buf.CurrentFrameMVWriteBufferAddressAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, buf.CurrentFrameMVWriteBufferAddress.bo, 0),
      };
      buf.IntraBCDecodedOutputFrameBufferAddressAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, NULL, 0),//DecodedOutputFrameBufferAddress.bo, 0),
      };

      if (use_internal_cache_mem) {
         buf.BitstreamLineRowstoreBufferAddress = (struct anv_address) { NULL, btdl_cache_offset * 64 };
         buf.BitstreamLineRowstoreBufferAddressAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
            .MOCS = anv_mocs(cmd_buffer->device, NULL, 0),
            .RowStoreScratchBufferCacheSelect = 1,
         };
      } else {
         buf.BitstreamLineRowstoreBufferAddress = (struct anv_address) { vid->vid_mem[ANV_VID_MEM_AV1_BITSTREAM_LINE_ROWSTORE].mem->bo,
                                                                         vid->vid_mem[ANV_VID_MEM_AV1_BITSTREAM_LINE_ROWSTORE].offset };
         buf.BitstreamLineRowstoreBufferAddressAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
            .MOCS = anv_mocs(cmd_buffer->device, vid->vid_mem[ANV_VID_MEM_AV1_BITSTREAM_LINE_ROWSTORE].mem->bo, 0),
         };
      }
      buf.BitstreamTileLineRowstoreBufferAddress = (struct anv_address) { vid->vid_mem[ANV_VID_MEM_AV1_BITSTREAM_TILE_LINE_ROWSTORE].mem->bo,
                                                   vid->vid_mem[ANV_VID_MEM_AV1_BITSTREAM_TILE_LINE_ROWSTORE].offset };
      buf.BitstreamTileLineRowstoreBufferAddressAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, vid->vid_mem[ANV_VID_MEM_AV1_BITSTREAM_TILE_LINE_ROWSTORE].mem->bo, 0),
      };

      if (use_internal_cache_mem) {
         buf.IntraPredictionLineRowstoreBufferAddress = (struct anv_address) { NULL, ipdl_cache_offset * 64 };
         buf.IntraPredictionLineRowstoreBufferAddressAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
            .MOCS = anv_mocs(cmd_buffer->device, NULL, 0),
            .RowStoreScratchBufferCacheSelect = 1
         };
      } else {
         buf.IntraPredictionLineRowstoreBufferAddress = (struct anv_address) { vid->vid_mem[ANV_VID_MEM_AV1_INTRA_PREDICTION_LINE_ROWSTORE].mem->bo,
                                                                               vid->vid_mem[ANV_VID_MEM_AV1_INTRA_PREDICTION_LINE_ROWSTORE].offset };
         buf.IntraPredictionLineRowstoreBufferAddressAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
            .MOCS = anv_mocs(cmd_buffer->device, vid->vid_mem[ANV_VID_MEM_AV1_INTRA_PREDICTION_LINE_ROWSTORE].mem->bo, 0),
         };
      }
      buf.IntraPredictionTileLineRowstoreBufferAddress = (struct anv_address) { vid->vid_mem[ANV_VID_MEM_AV1_INTRA_PREDICTION_TILE_LINE_ROWSTORE].mem->bo,
                                                   vid->vid_mem[ANV_VID_MEM_AV1_INTRA_PREDICTION_TILE_LINE_ROWSTORE].offset };
      buf.IntraPredictionTileLineRowstoreBufferAddressAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, vid->vid_mem[ANV_VID_MEM_AV1_INTRA_PREDICTION_TILE_LINE_ROWSTORE].mem->bo, 0),
      };

      if (use_internal_cache_mem) {
         buf.SpatialMotionVectorLineBufferAddress = (struct anv_address) { NULL, smvl_cache_offset * 64 };
         buf.SpatialMotionVectorLineBufferAddressAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
            .MOCS = anv_mocs(cmd_buffer->device, NULL, 0),
            .RowStoreScratchBufferCacheSelect = 1
         };
      } else {
         buf.SpatialMotionVectorLineBufferAddress = (struct anv_address) { vid->vid_mem[ANV_VID_MEM_AV1_SPATIAL_MOTION_VECTOR_LINE].mem->bo,
                                                                           vid->vid_mem[ANV_VID_MEM_AV1_SPATIAL_MOTION_VECTOR_LINE].offset };
         buf.SpatialMotionVectorLineBufferAddressAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
            .MOCS = anv_mocs(cmd_buffer->device, vid->vid_mem[ANV_VID_MEM_AV1_SPATIAL_MOTION_VECTOR_LINE].mem->bo, 0),
         };
      }
      buf.SpatialMotionVectorTileLineBufferAddress = (struct anv_address) { vid->vid_mem[ANV_VID_MEM_AV1_SPATIAL_MOTION_VECTOR_TILE_LINE].mem->bo,
                                                   vid->vid_mem[ANV_VID_MEM_AV1_SPATIAL_MOTION_VECTOR_TILE_LINE].offset };
      buf.SpatialMotionVectorTileLineBufferAddressAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, vid->vid_mem[ANV_VID_MEM_AV1_SPATIAL_MOTION_VECTOR_TILE_LINE].mem->bo, 0),
      };
      buf.LoopRestorationMetaTileColumnBufferAddress = (struct anv_address) { vid->vid_mem[ANV_VID_MEM_AV1_LOOP_RESTORATION_META_TILE_COLUMN].mem->bo,
                                                   vid->vid_mem[ANV_VID_MEM_AV1_LOOP_RESTORATION_META_TILE_COLUMN].offset };
      buf.LoopRestorationMetaTileColumnBufferAddressAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, vid->vid_mem[ANV_VID_MEM_AV1_LOOP_RESTORATION_META_TILE_COLUMN].mem->bo, 0),
      };
      buf.LoopRestorationFilterTileLineYBufferAddress = (struct anv_address) { vid->vid_mem[ANV_VID_MEM_AV1_LOOP_RESTORATION_FILTER_TILE_LINE_Y].mem->bo,
                                                   vid->vid_mem[ANV_VID_MEM_AV1_LOOP_RESTORATION_FILTER_TILE_LINE_Y].offset };
      buf.LoopRestorationFilterTileLineYBufferAddressAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, vid->vid_mem[ANV_VID_MEM_AV1_LOOP_RESTORATION_FILTER_TILE_LINE_Y].mem->bo, 0),
      };
      buf.LoopRestorationFilterTileLineUBufferAddress = (struct anv_address) { vid->vid_mem[ANV_VID_MEM_AV1_LOOP_RESTORATION_FILTER_TILE_LINE_U].mem->bo,
                                                   vid->vid_mem[ANV_VID_MEM_AV1_LOOP_RESTORATION_FILTER_TILE_LINE_U].offset };
      buf.LoopRestorationFilterTileLineUBufferAddressAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, vid->vid_mem[ANV_VID_MEM_AV1_LOOP_RESTORATION_FILTER_TILE_LINE_U].mem->bo, 0),
      };
      buf.LoopRestorationFilterTileLineVBufferAddress = (struct anv_address) { vid->vid_mem[ANV_VID_MEM_AV1_LOOP_RESTORATION_FILTER_TILE_LINE_V].mem->bo,
                                                   vid->vid_mem[ANV_VID_MEM_AV1_LOOP_RESTORATION_FILTER_TILE_LINE_V].offset };
      buf.LoopRestorationFilterTileLineVBufferAddressAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, vid->vid_mem[ANV_VID_MEM_AV1_LOOP_RESTORATION_FILTER_TILE_LINE_V].mem->bo, 0),
      };

      if (use_internal_cache_mem) {
         buf.DeblockerFilterLineYBufferAddress = (struct anv_address) { NULL, dfly_cache_offset * 64};
         buf.DeblockerFilterLineYBufferAddressAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
            .MOCS = anv_mocs(cmd_buffer->device, NULL, 0),
            .RowStoreScratchBufferCacheSelect = 1,
         };
      } else {
         buf.DeblockerFilterLineYBufferAddress = (struct anv_address) { vid->vid_mem[ANV_VID_MEM_AV1_DEBLOCKER_FILTER_LINE_Y].mem->bo,
                                                                        vid->vid_mem[ANV_VID_MEM_AV1_DEBLOCKER_FILTER_LINE_Y].offset };
         buf.DeblockerFilterLineYBufferAddressAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
            .MOCS = anv_mocs(cmd_buffer->device, vid->vid_mem[ANV_VID_MEM_AV1_DEBLOCKER_FILTER_LINE_Y].mem->bo, 0),
         };
      }

      if (use_internal_cache_mem) {
         buf.DeblockerFilterLineUBufferAddress = (struct anv_address) { NULL, dflu_cache_offset * 64 };
         buf.DeblockerFilterLineUBufferAddressAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
            .MOCS = anv_mocs(cmd_buffer->device, NULL, 0),
            .RowStoreScratchBufferCacheSelect = 1,
         };
      } else {
         buf.DeblockerFilterLineUBufferAddress = (struct anv_address) { vid->vid_mem[ANV_VID_MEM_AV1_DEBLOCKER_FILTER_LINE_U].mem->bo,
                                                                        vid->vid_mem[ANV_VID_MEM_AV1_DEBLOCKER_FILTER_LINE_U].offset };
         buf.DeblockerFilterLineUBufferAddressAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
            .MOCS = anv_mocs(cmd_buffer->device, vid->vid_mem[ANV_VID_MEM_AV1_DEBLOCKER_FILTER_LINE_U].mem->bo, 0),
         };
      }
      if (use_internal_cache_mem) {
         buf.DeblockerFilterLineVBufferAddress = (struct anv_address) { NULL, dflv_cache_offset * 64 };
         buf.DeblockerFilterLineVBufferAddressAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
            .MOCS = anv_mocs(cmd_buffer->device, NULL, 0),
            .RowStoreScratchBufferCacheSelect = 1,
         };
      } else {
         buf.DeblockerFilterLineVBufferAddress = (struct anv_address) { vid->vid_mem[ANV_VID_MEM_AV1_DEBLOCKER_FILTER_LINE_V].mem->bo,
                                                                        vid->vid_mem[ANV_VID_MEM_AV1_DEBLOCKER_FILTER_LINE_V].offset };
         buf.DeblockerFilterLineVBufferAddressAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
            .MOCS = anv_mocs(cmd_buffer->device, vid->vid_mem[ANV_VID_MEM_AV1_DEBLOCKER_FILTER_LINE_V].mem->bo, 0),
         };
      }

      buf.DeblockerFilterTileLineYBufferAddress = (struct anv_address) { vid->vid_mem[ANV_VID_MEM_AV1_DEBLOCKER_FILTER_TILE_LINE_Y].mem->bo,
                                                   vid->vid_mem[ANV_VID_MEM_AV1_DEBLOCKER_FILTER_TILE_LINE_Y].offset };
      buf.DeblockerFilterTileLineYBufferAddressAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, vid->vid_mem[ANV_VID_MEM_AV1_DEBLOCKER_FILTER_TILE_LINE_Y].mem->bo, 0),
      };
      buf.DeblockerFilterTileLineUBufferAddress = (struct anv_address) { vid->vid_mem[ANV_VID_MEM_AV1_DEBLOCKER_FILTER_TILE_LINE_U].mem->bo,
                                                   vid->vid_mem[ANV_VID_MEM_AV1_DEBLOCKER_FILTER_TILE_LINE_U].offset };
      buf.DeblockerFilterTileLineUBufferAddressAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, vid->vid_mem[ANV_VID_MEM_AV1_DEBLOCKER_FILTER_TILE_LINE_U].mem->bo, 0),
      };
      buf.DeblockerFilterTileLineVBufferAddress = (struct anv_address) { vid->vid_mem[ANV_VID_MEM_AV1_DEBLOCKER_FILTER_TILE_LINE_V].mem->bo,
                                                   vid->vid_mem[ANV_VID_MEM_AV1_DEBLOCKER_FILTER_TILE_LINE_V].offset };
      buf.DeblockerFilterTileLineVBufferAddressAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, vid->vid_mem[ANV_VID_MEM_AV1_DEBLOCKER_FILTER_TILE_LINE_V].mem->bo, 0),
      };
      buf.DeblockerFilterTileColumnYBufferAddress = (struct anv_address) { vid->vid_mem[ANV_VID_MEM_AV1_DEBLOCKER_FILTER_TILE_COLUMN_Y].mem->bo,
                                                   vid->vid_mem[ANV_VID_MEM_AV1_DEBLOCKER_FILTER_TILE_COLUMN_Y].offset };
      buf.DeblockerFilterTileColumnYBufferAddressAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, vid->vid_mem[ANV_VID_MEM_AV1_DEBLOCKER_FILTER_TILE_COLUMN_Y].mem->bo, 0),
      };
      buf.DeblockerFilterTileColumnUBufferAddress = (struct anv_address) { vid->vid_mem[ANV_VID_MEM_AV1_DEBLOCKER_FILTER_TILE_COLUMN_U].mem->bo,
                                                   vid->vid_mem[ANV_VID_MEM_AV1_DEBLOCKER_FILTER_TILE_COLUMN_U].offset };
      buf.DeblockerFilterTileColumnUBufferAddressAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, vid->vid_mem[ANV_VID_MEM_AV1_DEBLOCKER_FILTER_TILE_COLUMN_U].mem->bo, 0),
      };
      buf.DeblockerFilterTileColumnVBufferAddress = (struct anv_address) { vid->vid_mem[ANV_VID_MEM_AV1_DEBLOCKER_FILTER_TILE_COLUMN_V].mem->bo,
                                                   vid->vid_mem[ANV_VID_MEM_AV1_DEBLOCKER_FILTER_TILE_COLUMN_V].offset };
      buf.DeblockerFilterTileColumnVBufferAddressAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, vid->vid_mem[ANV_VID_MEM_AV1_DEBLOCKER_FILTER_TILE_COLUMN_V].mem->bo, 0),
      };

      if (use_internal_cache_mem) {
         buf.CDEFFilterLineBufferAddress = (struct anv_address) { NULL, cdef_cache_offset * 64};
         buf.CDEFFilterLineBufferAddressAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
            .MOCS = anv_mocs(cmd_buffer->device, NULL, 0),
            .RowStoreScratchBufferCacheSelect = 1,
         };
      } else {
         buf.CDEFFilterLineBufferAddress = (struct anv_address) { vid->vid_mem[ANV_VID_MEM_AV1_CDEF_FILTER_LINE].mem->bo,
                                                                  vid->vid_mem[ANV_VID_MEM_AV1_CDEF_FILTER_LINE].offset };
         buf.CDEFFilterLineBufferAddressAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
            .MOCS = anv_mocs(cmd_buffer->device, vid->vid_mem[ANV_VID_MEM_AV1_CDEF_FILTER_LINE].mem->bo, 0),
         };
      }

      buf.CDEFFilterTileLineBufferAddress = (struct anv_address) { vid->vid_mem[ANV_VID_MEM_AV1_CDEF_FILTER_TILE_LINE].mem->bo,
                                                   vid->vid_mem[ANV_VID_MEM_AV1_CDEF_FILTER_TILE_LINE].offset };
      buf.CDEFFilterTileLineBufferAddressAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, vid->vid_mem[ANV_VID_MEM_AV1_CDEF_FILTER_TILE_LINE].mem->bo, 0),
      };
      buf.CDEFFilterTileColumnBufferAddress = (struct anv_address) { vid->vid_mem[ANV_VID_MEM_AV1_CDEF_FILTER_TILE_COLUMN].mem->bo,
                                                   vid->vid_mem[ANV_VID_MEM_AV1_CDEF_FILTER_TILE_COLUMN].offset };
      buf.CDEFFilterTileColumnBufferAddressAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, vid->vid_mem[ANV_VID_MEM_AV1_CDEF_FILTER_TILE_COLUMN].mem->bo, 0),
      };
      buf.CDEFFilterMetaTileLineBufferAddress = (struct anv_address) { vid->vid_mem[ANV_VID_MEM_AV1_CDEF_FILTER_META_TILE_LINE].mem->bo,
                                                   vid->vid_mem[ANV_VID_MEM_AV1_CDEF_FILTER_META_TILE_LINE].offset };
      buf.CDEFFilterMetaTileLineBufferAddressAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, vid->vid_mem[ANV_VID_MEM_AV1_CDEF_FILTER_META_TILE_LINE].mem->bo, 0),
      };
      buf.CDEFFilterMetaTileColumnBufferAddress = (struct anv_address) { vid->vid_mem[ANV_VID_MEM_AV1_CDEF_FILTER_META_TILE_COLUMN].mem->bo,
                                                   vid->vid_mem[ANV_VID_MEM_AV1_CDEF_FILTER_META_TILE_COLUMN].offset };
      buf.CDEFFilterMetaTileColumnBufferAddressAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, vid->vid_mem[ANV_VID_MEM_AV1_CDEF_FILTER_META_TILE_COLUMN].mem->bo, 0),
      };
      buf.CDEFFilterTopLeftCornerBufferAddress = (struct anv_address) { vid->vid_mem[ANV_VID_MEM_AV1_CDEF_FILTER_TOP_LEFT_CORNER].mem->bo,
                                                   vid->vid_mem[ANV_VID_MEM_AV1_CDEF_FILTER_TOP_LEFT_CORNER].offset };
      buf.CDEFFilterTopLeftCornerBufferAddressAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, vid->vid_mem[ANV_VID_MEM_AV1_CDEF_FILTER_TOP_LEFT_CORNER].mem->bo, 0),
      };
      buf.SuperResTileColumnYBufferAddress = (struct anv_address) { vid->vid_mem[ANV_VID_MEM_AV1_SUPER_RES_TILE_COLUMN_Y].mem->bo,
                                                   vid->vid_mem[ANV_VID_MEM_AV1_SUPER_RES_TILE_COLUMN_Y].offset };
      buf.SuperResTileColumnYBufferAddressAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, vid->vid_mem[ANV_VID_MEM_AV1_SUPER_RES_TILE_COLUMN_Y].mem->bo, 0),
      };
      buf.SuperResTileColumnUBufferAddress = (struct anv_address) { vid->vid_mem[ANV_VID_MEM_AV1_SUPER_RES_TILE_COLUMN_U].mem->bo,
                                                   vid->vid_mem[ANV_VID_MEM_AV1_SUPER_RES_TILE_COLUMN_U].offset };
      buf.SuperResTileColumnUBufferAddressAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, vid->vid_mem[ANV_VID_MEM_AV1_SUPER_RES_TILE_COLUMN_U].mem->bo, 0),
      };
      buf.SuperResTileColumnVBufferAddress = (struct anv_address) { vid->vid_mem[ANV_VID_MEM_AV1_SUPER_RES_TILE_COLUMN_V].mem->bo,
                                                   vid->vid_mem[ANV_VID_MEM_AV1_SUPER_RES_TILE_COLUMN_V].offset };
      buf.SuperResTileColumnVBufferAddressAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, vid->vid_mem[ANV_VID_MEM_AV1_SUPER_RES_TILE_COLUMN_V].mem->bo, 0),
      };
      buf.LoopRestorationFilterTileColumnYBufferAddress = (struct anv_address) { vid->vid_mem[ANV_VID_MEM_AV1_LOOP_RESTORATION_FILTER_TILE_COLUMN_Y].mem->bo,
                                                   vid->vid_mem[ANV_VID_MEM_AV1_LOOP_RESTORATION_FILTER_TILE_COLUMN_Y].offset };
      buf.LoopRestorationFilterTileColumnYBufferAddressAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, vid->vid_mem[ANV_VID_MEM_AV1_LOOP_RESTORATION_FILTER_TILE_COLUMN_Y].mem->bo, 0),
      };
      buf.LoopRestorationFilterTileColumnUBufferAddress = (struct anv_address) { vid->vid_mem[ANV_VID_MEM_AV1_LOOP_RESTORATION_FILTER_TILE_COLUMN_U].mem->bo,
                                                   vid->vid_mem[ANV_VID_MEM_AV1_LOOP_RESTORATION_FILTER_TILE_COLUMN_U].offset };
      buf.LoopRestorationFilterTileColumnUBufferAddressAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, vid->vid_mem[ANV_VID_MEM_AV1_LOOP_RESTORATION_FILTER_TILE_COLUMN_U].mem->bo, 0),
      };
      buf.LoopRestorationFilterTileColumnVBufferAddress = (struct anv_address) { vid->vid_mem[ANV_VID_MEM_AV1_LOOP_RESTORATION_FILTER_TILE_COLUMN_V].mem->bo,
                                                   vid->vid_mem[ANV_VID_MEM_AV1_LOOP_RESTORATION_FILTER_TILE_COLUMN_V].offset };
      buf.LoopRestorationFilterTileColumnVBufferAddressAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, vid->vid_mem[ANV_VID_MEM_AV1_LOOP_RESTORATION_FILTER_TILE_COLUMN_V].mem->bo, 0),
      };

      struct anv_bo *ref_bo = NULL;
      struct anv_bo *collocated_bo = NULL;
      for (enum av1_ref_frame r = AV1_INTRA_FRAME; r <= AV1_ALTREF_FRAME; r++) {
         const struct anv_image *ref_img = ref_info[r].img;
         if (ref_img) {
            buf.ReferencePictureAddress[r] =  anv_image_address(ref_img,
                                                                &ref_img->planes[0].primary_surface.memory_range);
            buf.CollocatedMVTemporalBufferAddress[r] = anv_image_address(ref_img,
                                                                      &ref_img->vid_dmv_top_surface);
            if (!ref_bo)
               ref_bo = ref_img->bindings[0].address.bo;
            if (!collocated_bo)
               collocated_bo = ref_img->bindings[ref_img->vid_dmv_top_surface.binding].address.bo;
         }
      }

      buf.ReferencePictureAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, ref_bo, 0),
#if GFX_VERx10 >= 125
         .TiledResourceMode = TRMODE_TILEF,
#endif
      };
      buf.CollocatedMVTemporalBufferAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, collocated_bo, 0),
      };

      bool use_default_cdf = false;
      if (av1_pic_info->frame_header->primary_ref_frame == 7) {
         use_default_cdf = true;
      } else {
         if (ref_info[av1_pic_info->frame_header->primary_ref_frame + 1].cdf_update_disabled)
            use_default_cdf = true;
      }

      if (use_default_cdf) {
         buf.CDFTablesInitializationBufferAddress = (struct anv_address) {
            vid->vid_mem[ANV_VID_MEM_AV1_CDF_DEFAULTS_0 + cdf_index].mem->bo,
            vid->vid_mem[ANV_VID_MEM_AV1_CDF_DEFAULTS_0 + cdf_index].offset };
      } else {
         const struct anv_image *ref_img = ref_info[av1_pic_info->frame_header->primary_ref_frame + 1].img;
         buf.CDFTablesInitializationBufferAddress = anv_image_address(ref_img,
                                                                      &ref_img->av1_cdf_table);
      }
      buf.CDFTablesInitializationBufferAddressAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, buf.CDFTablesInitializationBufferAddress.bo, 0),
      };

      if (!av1_pic_info->frame_header->flags.disable_frame_end_update_cdf) {
         const struct anv_image *ref_img = ref_info[0].img;
         buf.CDFTablesBackwardAdaptationBufferAddress = anv_image_address(ref_img,
                                                                          &ref_img->av1_cdf_table);
      }

      buf.CDFTablesBackwardAdaptationBufferAddressAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, buf.CDFTablesBackwardAdaptationBufferAddress.bo, 0),
      };
      buf.AV1SegmentIDReadBufferAddressAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, NULL, 0),
      };
      buf.AV1SegmentIDWriteBufferAddressAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, NULL, 0),
      };

      buf.DecodedFrameStatusErrorBufferAddressAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, NULL, 0),
      };
      buf.DecodedBlockDataStreamoutBufferAddress = (struct anv_address) { vid->vid_mem[ANV_VID_MEM_AV1_DBD_BUFFER].mem->bo,
                                                                          vid->vid_mem[ANV_VID_MEM_AV1_DBD_BUFFER].offset };
      buf.DecodedBlockDataStreamoutBufferAddressAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, vid->vid_mem[ANV_VID_MEM_AV1_DBD_BUFFER].mem->bo, 0),
      };
   };

   anv_batch_emit(&cmd_buffer->batch, GENX(AVP_IND_OBJ_BASE_ADDR_STATE), ind) {
      ind.AVPIndirectBitstreamObjectBaseAddress = anv_address_add(src_buffer->address,
                                                                  frame_info->srcBufferOffset);
      ind.AVPIndirectBitstreamObjectAddressAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, src_buffer->address.bo, 0),
      };
#if GFX_VERx10 >= 125
      ind.AVPIndirectCUObjectAddressAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd_buffer->device, NULL, 0),
      };
#endif
   }

   enum {
      AV1_RESTORE_NONE       = 0,
      AV1_RESTORE_WIENER     = 1,
      AV1_RESTORE_SGRPROJ    = 2,
      AV1_RESTORE_SWITCHABLE = 3,
   };
   uint8_t remap_lr_type[4] = {AV1_RESTORE_NONE, AV1_RESTORE_SWITCHABLE, AV1_RESTORE_WIENER, AV1_RESTORE_SGRPROJ};
   uint32_t frame_restoration_type[3];
   frame_restoration_type[0] = remap_lr_type[av1_pic_info->frame_header->lr.lr_type[0]];
   frame_restoration_type[1] = remap_lr_type[av1_pic_info->frame_header->lr.lr_type[1]];
   frame_restoration_type[2] = remap_lr_type[av1_pic_info->frame_header->lr.lr_type[2]];

   uint32_t ref_mask = 0;
   uint32_t ref_frame_sign_bias = 0;
   uint32_t ref_frame_side = 0;
   for (enum av1_ref_frame r = AV1_LAST_FRAME; r <= AV1_ALTREF_FRAME; r++) {
      if (params->vk.av1_dec.seq_hdr.flags.enable_order_hint &&
          !frame_is_key_or_intra(av1_pic_info)) {
         if (get_relative_dist(av1_pic_info, params,
                               ref_info[r].order_hint, ref_info[AV1_INTRA_FRAME].order_hint) > 0)
            ref_frame_sign_bias |= (1 << r);

         if ((get_relative_dist(av1_pic_info, params,
                                ref_info[r].order_hint, ref_info[AV1_INTRA_FRAME].order_hint) > 0) ||
             ref_info[r].order_hint == ref_info[AV1_INTRA_FRAME].order_hint)
            ref_frame_side |= (1 << r);
      }
   }

   uint8_t num_mfmv = 0;
   uint8_t mfmv_ref[7] = {};
   if (av1_pic_info->frame_header->flags.use_ref_frame_mvs &&
       params->vk.av1_dec.seq_hdr.order_hint_bits_minus_1 + 1) {
      int total = 2;
      if (av1_pic_info->frame_header->ref_frame_idx[AV1_LAST_FRAME - AV1_LAST_FRAME] >= 0) {
         if (ref_info[AV1_LAST_FRAME].ref_order_hints[AV1_ALTREF_FRAME - AV1_LAST_FRAME] != ref_info[AV1_GOLDEN_FRAME].order_hint) {
            total = 3;
            mfmv_ref[num_mfmv++] = AV1_LAST_FRAME - AV1_LAST_FRAME;
         }
      }

      if (av1_pic_info->frame_header->ref_frame_idx[AV1_BWDREF_FRAME - AV1_LAST_FRAME] >= 0 &&
          get_relative_dist(av1_pic_info, params,
                            ref_info[AV1_BWDREF_FRAME].order_hint,
                            ref_info[AV1_INTRA_FRAME].order_hint) > 0)
         mfmv_ref[num_mfmv++] = AV1_BWDREF_FRAME - AV1_LAST_FRAME;

      if (av1_pic_info->frame_header->ref_frame_idx[AV1_ALTREF2_FRAME - AV1_LAST_FRAME] >= 0 &&
          get_relative_dist(av1_pic_info, params,
                            ref_info[AV1_ALTREF2_FRAME].order_hint,
                            ref_info[AV1_INTRA_FRAME].order_hint) > 0)
         mfmv_ref[num_mfmv++] = AV1_ALTREF2_FRAME - AV1_LAST_FRAME;

      if (num_mfmv < total && av1_pic_info->frame_header->ref_frame_idx[AV1_ALTREF_FRAME - AV1_LAST_FRAME] >= 0 &&
          get_relative_dist(av1_pic_info, params,
                            ref_info[AV1_ALTREF_FRAME].order_hint,
                            ref_info[AV1_INTRA_FRAME].order_hint) > 0)
         mfmv_ref[num_mfmv++] = AV1_ALTREF_FRAME - AV1_LAST_FRAME;

      if (num_mfmv < total &&
          av1_pic_info->frame_header->ref_frame_idx[AV1_LAST2_FRAME - AV1_LAST_FRAME] >= 0)
         mfmv_ref[num_mfmv++] = AV1_LAST2_FRAME - AV1_LAST_FRAME;
   }

   for (unsigned int i = 0; i < num_mfmv; i++) {
      ref_mask |= (1 << mfmv_ref[i]);
   }

   uint8_t preskip_segid = 0;
   uint8_t last_active_segid = 0;
   bool frame_lossless = true;
   bool lossless[8] = { false };

   for (unsigned i = 0; i < 8; i++) {
      for (unsigned j = 0; j < 8; j++) {
         if (av1_pic_info->frame_header->segmentation.feature_enabled_bits[i] & (1 << j)) {
            last_active_segid = i;
            if (j >= 5)
               preskip_segid = 1;
         }
      }
      uint32_t qindex = get_qindex(av1_pic_info, i);
      lossless[i] = (qindex == 0) &&
         (av1_pic_info->frame_header->quantization.delta_q_y_dc == 0) &&
         (av1_pic_info->frame_header->quantization.delta_q_u_ac == 0) &&
         (av1_pic_info->frame_header->quantization.delta_q_u_dc == 0) &&
         (av1_pic_info->frame_header->quantization.delta_q_v_ac == 0) &&
         (av1_pic_info->frame_header->quantization.delta_q_v_dc == 0);
      frame_lossless &= lossless[i];
   }

   anv_batch_emit(&cmd_buffer->batch, GENX(AVP_PIC_STATE), pic) {
      pic.FrameWidth = av1_pic_info->frame_header->frame_width_minus_1;
      pic.FrameHeight = av1_pic_info->frame_header->frame_height_minus_1;
      if (params->vk.av1_dec.seq_hdr.color_config.bit_depth == 12)
         pic.SequencePixelBitDepthIdc = SeqPix_12bit;
      else if (params->vk.av1_dec.seq_hdr.color_config.bit_depth == 10)
         pic.SequencePixelBitDepthIdc = SeqPix_10bit;
      else
         pic.SequencePixelBitDepthIdc = SeqPix_8bit;
      if (params->vk.av1_dec.seq_hdr.color_config.subsampling_x == 1 &&
          params->vk.av1_dec.seq_hdr.color_config.subsampling_y == 1) {
         if (params->vk.av1_dec.seq_hdr.color_config.flags.mono_chrome)
            pic.SequenceChromaSubSamplingFormat = SS_Monochrome;
         else
            pic.SequenceChromaSubSamplingFormat = SS_420;
      } else if (params->vk.av1_dec.seq_hdr.color_config.subsampling_x == 1 &&
                 params->vk.av1_dec.seq_hdr.color_config.subsampling_y == 0) {
         pic.SequenceChromaSubSamplingFormat = SS_422;
      } else if (params->vk.av1_dec.seq_hdr.color_config.subsampling_x == 0 &&
                 params->vk.av1_dec.seq_hdr.color_config.subsampling_y == 0) {
         pic.SequenceChromaSubSamplingFormat = SS_444;
      }
      pic.SequenceSuperblockSizeUsed = params->vk.av1_dec.seq_hdr.flags.use_128x128_superblock;
      pic.SequenceEnableOrderHintFlag = params->vk.av1_dec.seq_hdr.flags.enable_order_hint;
      pic.SequenceOrderHintBitsMinus1 = params->vk.av1_dec.seq_hdr.flags.enable_order_hint ? params->vk.av1_dec.seq_hdr.order_hint_bits_minus_1 : 0;
      pic.SequenceEnableFilterIntraFlag = params->vk.av1_dec.seq_hdr.flags.enable_filter_intra;
      pic.SequenceEnableIntraEdgeFilterFlag = params->vk.av1_dec.seq_hdr.flags.enable_intra_edge_filter;
      pic.SequenceEnableDualFilterFlag = params->vk.av1_dec.seq_hdr.flags.enable_dual_filter;
      pic.SequenceEnableInterIntraCompoundFlag = params->vk.av1_dec.seq_hdr.flags.enable_interintra_compound;
      pic.SequenceEnableMaskedCompoundFlag = params->vk.av1_dec.seq_hdr.flags.enable_masked_compound;
      pic.SequenceEnableJointCompoundFlag = params->vk.av1_dec.seq_hdr.flags.enable_jnt_comp;
      pic.AllowScreenContentToolsFlag = av1_pic_info->frame_header->flags.allow_screen_content_tools;
      pic.ForceIntegerMVFlag = av1_pic_info->frame_header->flags.force_integer_mv;
      pic.AllowWarpedMotionFlag = av1_pic_info->frame_header->flags.allow_warped_motion;
      pic.UseCDEFFilterFlag = params->vk.av1_dec.seq_hdr.flags.enable_cdef;
      pic.UseSuperResFlag = av1_pic_info->frame_header->flags.use_superres;
      pic.FrameLevelLoopRestorationFilterEnable = frame_restoration_type[0] || frame_restoration_type[1] || frame_restoration_type[2];
      pic.FrameType = av1_pic_info->frame_header->frame_type;
      pic.IntraOnlyFlag = frame_is_key_or_intra(av1_pic_info);
      pic.ErrorResilientModeFlag = av1_pic_info->frame_header->flags.error_resilient_mode;
      pic.AllowIntraBCFlag = av1_pic_info->frame_header->flags.allow_intrabc;
      pic.PrimaryReferenceFrameIdx = av1_pic_info->frame_header->primary_ref_frame;
      pic.SegmentationEnableFlag = av1_pic_info->frame_header->segmentation.flags.segmentation_enabled;
      pic.SegmentationUpdateMapFlag = av1_pic_info->frame_header->segmentation.flags.segmentation_update_map;
      pic.SegmentationTemporalUpdateFlag = pic.IntraOnlyFlag ? 0 : av1_pic_info->frame_header->segmentation.flags.segmentation_temporal_update;
      pic.PreSkipSegmentIDFlag = preskip_segid;
      pic.LastActiveSegmentSegmentID = last_active_segid;
      pic.DeltaQPresentFlag = av1_pic_info->frame_header->flags.delta_q_present;
      pic.DeltaQRes = av1_pic_info->frame_header->delta_q.delta_q_res;
      pic.FrameCodedLosslessMode = frame_lossless;//TODO
      pic.SegmentMapisZeroFlag = 0;//TODO
      pic.SegmentIDBufferStreamInEnableFlag = 0;//TODO
      pic.SegmentIDBufferStreamOutEnableFlag = 0;//TODO
      pic.BaseQindex = av1_pic_info->frame_header->quantization.base_q_idx;
      pic.YdcdeltaQ = av1_pic_info->frame_header->quantization.delta_q_y_dc;
      pic.UdcdeltaQ = av1_pic_info->frame_header->quantization.delta_q_u_dc;
      pic.UacdeltaQ = av1_pic_info->frame_header->quantization.delta_q_u_ac;
      pic.VdcdeltaQ = av1_pic_info->frame_header->quantization.delta_q_v_dc;
      pic.VacdeltaQ = av1_pic_info->frame_header->quantization.delta_q_v_ac;
      pic.AllowHighPrecisionMV = av1_pic_info->frame_header->flags.allow_high_precision_mv;
      pic.FrameLevelReferenceModeSelect = !(av1_pic_info->frame_header->flags.reference_select == 0);
      pic.McompFilterType = av1_pic_info->frame_header->interpolation_filter;
      pic.MotionModeSwitchableFlag = av1_pic_info->frame_header->flags.is_motion_mode_switchable;
      pic.UseReferenceFrameMVSetFlag = av1_pic_info->frame_header->flags.use_ref_frame_mvs;
      pic.ReferenceFrameSignBias = ref_frame_sign_bias;
      pic.CurrentFrameOrderHint = av1_pic_info->frame_header->order_hint;
      pic.ReducedTxSetUsed = av1_pic_info->frame_header->flags.reduced_tx_set;
      pic.FrameTransformMode = av1_pic_info->frame_header->tx_mode;
      pic.SkipModePresentFlag = av1_pic_info->frame_header->flags.skip_mode_present;
      pic.SkipModeFrame0 = av1_pic_info->frame_header->flags.skip_mode_present ? av1_pic_info->skip_mode_frame_idx[0] : 0;
      pic.SkipModeFrame1 = av1_pic_info->frame_header->flags.skip_mode_present ? av1_pic_info->skip_mode_frame_idx[1] : 0;
      pic.ReferenceFrameSide = ref_frame_side;
      pic.GlobalMotionType1 = av1_pic_info->frame_header->global_motion[1].gm_type;
      pic.GlobalMotionType2 = av1_pic_info->frame_header->global_motion[2].gm_type;
      pic.GlobalMotionType3 = av1_pic_info->frame_header->global_motion[3].gm_type;
      pic.GlobalMotionType4 = av1_pic_info->frame_header->global_motion[4].gm_type;
      pic.GlobalMotionType5 = av1_pic_info->frame_header->global_motion[5].gm_type;
      pic.GlobalMotionType6 = av1_pic_info->frame_header->global_motion[6].gm_type;
      pic.GlobalMotionType7 = av1_pic_info->frame_header->global_motion[7].gm_type;

      pic.FrameLevelGlobalMotionInvalidFlags = 0;
      uint8_t idx = 0;
      for (enum av1_ref_frame r = AV1_LAST_FRAME; r <= AV1_ALTREF_FRAME; r++) {
         pic.FrameLevelGlobalMotionInvalidFlags |= av1_pic_info->frame_header->global_motion[r].flags.gm_invalid << r;

         for (uint32_t i = 0; i < 6; i++)
            pic.WarpParameters[idx++] = av1_pic_info->frame_header->global_motion[r].gm_params[i];
      }
      pic.ReferenceFrameIdx1 = AV1_LAST_FRAME;
      pic.ReferenceFrameIdx2 = AV1_LAST2_FRAME;
      pic.ReferenceFrameIdx3 = AV1_LAST3_FRAME;
      pic.ReferenceFrameIdx4 = AV1_GOLDEN_FRAME;
      pic.ReferenceFrameIdx5 = AV1_BWDREF_FRAME;
      pic.ReferenceFrameIdx6 = AV1_ALTREF2_FRAME;
      pic.ReferenceFrameIdx7 = AV1_ALTREF_FRAME;

      uint32_t cur_frame_width = av1_pic_info->frame_header->frame_width_minus_1;
      uint32_t cur_frame_height = av1_pic_info->frame_header->frame_height_minus_1;

      for (enum av1_ref_frame r = AV1_INTRA_FRAME; r <= AV1_ALTREF_FRAME; r++) {
         const struct anv_image *ref_img = ref_info[r].img;

         if (!ref_img)
             continue;

         int ref_width = ref_img->vk.extent.width - 1;
         int ref_height = ref_img->vk.extent.height - 1;

         uint32_t h_scale_factor = (ref_width * av1_scaling_factor + (cur_frame_width >> 1)) / cur_frame_width;
         uint32_t v_scale_factor = (ref_height * av1_scaling_factor + (cur_frame_height >> 1)) / cur_frame_height;
         switch (r) {
         case AV1_INTRA_FRAME:
            pic.IntraFrameWidthinPixelMinus1 = av1_pic_info->frame_header->frame_width_minus_1;
            pic.IntraFrameHeightinPixelMinus1 = av1_pic_info->frame_header->frame_height_minus_1;
            pic.VerticalScaleFactorForIntra = av1_scaling_factor;
            pic.HorizontalScaleFactorForIntra = av1_scaling_factor;
            break;
         case AV1_LAST_FRAME:
            pic.LastFrameWidthinPixelMinus1 = ref_width;
            pic.LastFrameHeightinPixelMinus1 = ref_height;
            pic.VerticalScaleFactorForLast = v_scale_factor;
            pic.HorizontalScaleFactorForLast = h_scale_factor;
            break;
         case AV1_LAST2_FRAME:
            pic.Last2FrameWidthinPixelMinus1 = ref_width;
            pic.Last2FrameHeightinPixelMinus1 = ref_height;
            pic.VerticalScaleFactorForLast2 = v_scale_factor;
            pic.HorizontalScaleFactorForLast2 = h_scale_factor;
            break;
         case AV1_LAST3_FRAME:
            pic.Last3FrameWidthinPixelMinus1 = ref_width;
            pic.Last3FrameHeightinPixelMinus1 = ref_height;
            pic.VerticalScaleFactorForLast3 = v_scale_factor;
            pic.HorizontalScaleFactorForLast3 = h_scale_factor;
            break;
         case AV1_GOLDEN_FRAME:
            pic.GoldenFrameWidthinPixelMinus1 = ref_width;
            pic.GoldenFrameHeightinPixelMinus1 = ref_height;
            pic.VerticalScaleFactorForGolden = v_scale_factor;
            pic.HorizontalScaleFactorForGolden = h_scale_factor;
            break;
         case AV1_BWDREF_FRAME:
            pic.BWDREFFrameWidthinPixelMinus1 = ref_width;
            pic.BWDREFFrameHeightinPixelMinus1 = ref_height;
            pic.VerticalScaleFactorForBWDREF = v_scale_factor;
            pic.HorizontalScaleFactorForBWDREF = h_scale_factor;
            break;
         case AV1_ALTREF2_FRAME:
            pic.ALTREF2FrameWidthinPixelMinus1 = ref_width;
            pic.ALTREF2FrameHeightinPixelMinus1 = ref_height;
            pic.VerticalScaleFactorForALTREF2 = v_scale_factor;
            pic.HorizontalScaleFactorForALTREF2 = h_scale_factor;
            break;
         case AV1_ALTREF_FRAME:
            pic.ALTREFFrameWidthinPixelMinus1 = ref_width;
            pic.ALTREFFrameHeightinPixelMinus1 = ref_height;
            pic.VerticalScaleFactorForALTREF = v_scale_factor;
            pic.HorizontalScaleFactorForALTREF = h_scale_factor;
            break;
         default:
            break;
         }
      }

      pic.FrameLevelGlobalMotionInvalidFlags = 0;
      for (enum av1_ref_frame r = AV1_INTRA_FRAME; r <= AV1_ALTREF_FRAME; r++)
         pic.ReferenceFrameOrderHint[r] = ref_info[r].order_hint;
   };

   anv_batch_emit(&cmd_buffer->batch, GENX(AVP_INTER_PRED_STATE), inter) {
      inter.ActiveReferenceBitmask = ref_mask;

      for (enum av1_ref_frame r = AV1_LAST_FRAME; r <= AV1_ALTREF_FRAME; r++) {
         switch (r) {
         case AV1_LAST_FRAME:
            for (unsigned j = 0; j < 7; j++)
               inter.SavedOrderHints0[j] = ref_info[r].ref_order_hints[j];
            break;
         case AV1_LAST2_FRAME:
            for (unsigned j = 0; j < 7; j++)
               inter.SavedOrderHints1[j] = ref_info[r].ref_order_hints[j];
            break;
         case AV1_LAST3_FRAME:
            for (unsigned j = 0; j < 7; j++)
               inter.SavedOrderHints2[j] = ref_info[r].ref_order_hints[j];
            break;
         case AV1_GOLDEN_FRAME:
            for (unsigned j = 0; j < 7; j++)
               inter.SavedOrderHints3[j] = ref_info[r].ref_order_hints[j];
            break;
         case AV1_BWDREF_FRAME:
            for (unsigned j = 0; j < 7; j++)
               inter.SavedOrderHints4[j] = ref_info[r].ref_order_hints[j];
            break;
         case AV1_ALTREF2_FRAME:
            for (unsigned j = 0; j < 7; j++)
               inter.SavedOrderHints5[j] = ref_info[r].ref_order_hints[j];
            break;
         case AV1_ALTREF_FRAME:
            for (unsigned j = 0; j < 7; j++)
               inter.SavedOrderHints6[j] = ref_info[r].ref_order_hints[j];
            break;
         default:
            break;
         }
      }
   }

   for (unsigned i = 0; i < 8; ++i) {
      anv_batch_emit(&cmd_buffer->batch, GENX(AVP_SEGMENT_STATE), seg) {
         seg.SegmentID = i;
         seg.SegmentFeatureMask = av1_pic_info->frame_header->segmentation.feature_enabled_bits[i];
         seg.SegmentDeltaQindex = av1_pic_info->frame_header->segmentation.feature_data[i][SEG_LVL_ALT_Q];
         seg.SegmentBlockSkipFlag = av1_pic_info->frame_header->segmentation.feature_data[i][SEG_LVL_SKIP];
         seg.SegmentBlockGlobalMVFlag = av1_pic_info->frame_header->segmentation.feature_data[i][SEG_LVL_GLOBAL_MV];
         seg.SegmentLosslessFlag = lossless[i];
         if (lossless[i] || !av1_pic_info->frame_header->quantization.flags.using_qmatrix) {
            seg.SegmentLumaYQMLevel = av1_num_qm_levels - 1;
            seg.SegmentChromaUQMLevel = av1_num_qm_levels - 1;
            seg.SegmentChromaVQMLevel = av1_num_qm_levels - 1;
         } else {
            seg.SegmentLumaYQMLevel = av1_pic_info->frame_header->quantization.qm_y;
            seg.SegmentChromaUQMLevel = av1_pic_info->frame_header->quantization.qm_u;
            seg.SegmentChromaVQMLevel = av1_pic_info->frame_header->quantization.qm_v;
         }
         seg.SegmentDeltaLoopFilterLevelLumaVertical = av1_pic_info->frame_header->segmentation.feature_data[i][SEG_LVL_ALT_LFYV];
         seg.SegmentDeltaLoopFilterLevelLumaHorizontal = av1_pic_info->frame_header->segmentation.feature_data[i][SEG_LVL_ALT_LFYH];
         seg.SegmentDeltaLoopFilterLevelChromaU = av1_pic_info->frame_header->segmentation.feature_data[i][SEG_LVL_ALT_LFU];
         seg.SegmentDeltaLoopFilterLevelChromaV = av1_pic_info->frame_header->segmentation.feature_data[i][SEG_LVL_ALT_LFV];
         seg.SegmentReferenceFrame = av1_pic_info->frame_header->segmentation.feature_data[i][SEG_LVL_REF_FRAME];
      };

      if (!av1_pic_info->frame_header->segmentation.flags.segmentation_enabled)
          break;
   }

   StdVideoAV1MESALoopFilter *lf = &av1_pic_info->frame_header->loop_filter;
   StdVideoAV1MESACDEF *cdef = &av1_pic_info->frame_header->cdef;
   uint32_t cdef_strengths[8] = { 0 }, cdef_uv_strengths[8] = { 0 };
   for (unsigned i = 0; i < (1 << cdef->cdef_bits); ++i) {
      cdef_strengths[i] = (cdef->cdef_y_pri_strength[i] << 2) +
         cdef->cdef_y_sec_strength[i];
      cdef_uv_strengths[i] = (cdef->cdef_uv_pri_strength[i] << 2) +
         cdef->cdef_uv_sec_strength[i];
   }

   anv_batch_emit(&cmd_buffer->batch, GENX(AVP_INLOOP_FILTER_STATE), fil) {
      fil.LumaYDeblockerFilterLevelVertical = lf->level[0];
      fil.LumaYDeblockerFilterLevelHorizontal = lf->level[1];
      fil.ChromaUDeblockerFilterLevel = lf->level[2];
      fil.ChromaVDeblockerFilterLevel = lf->level[3];
      fil.DeblockerFilterSharpnessLevel = lf->sharpness;
      fil.DeblockerFilterModeRefDeltaEnableFlag = lf->flags.loop_filter_delta_enabled;
      fil.DeblockerDeltaLFResolution = av1_pic_info->frame_header->delta_q.delta_lf_res;
      fil.DeblockerFilterDeltaLFMultiFlag = av1_pic_info->frame_header->delta_q.flags.delta_lf_multi;
      fil.DeblockerFilterDeltaLFPresentFlag = av1_pic_info->frame_header->delta_q.flags.delta_lf_present;
      fil.DeblockerFilterRefDeltas0 = lf->ref_deltas[0];
      fil.DeblockerFilterRefDeltas1 = lf->ref_deltas[1];
      fil.DeblockerFilterRefDeltas2 = lf->ref_deltas[2];
      fil.DeblockerFilterRefDeltas3 = lf->ref_deltas[3];
      fil.DeblockerFilterRefDeltas4 = lf->ref_deltas[4];
      fil.DeblockerFilterRefDeltas5 = lf->ref_deltas[5];
      fil.DeblockerFilterRefDeltas6 = lf->ref_deltas[6];
      fil.DeblockerFilterRefDeltas7 = lf->ref_deltas[7];
      fil.DeblockerFilterModeDeltas0 = lf->mode_deltas[0];
      fil.DeblockerFilterModeDeltas1 = lf->mode_deltas[1];
      fil.CDEFYStrength0 = cdef_strengths[0];
      fil.CDEFYStrength1 = cdef_strengths[1];
      fil.CDEFYStrength2 = cdef_strengths[2];
      fil.CDEFYStrength3 = cdef_strengths[3];
      fil.CDEFBits = cdef->cdef_bits;
      fil.CDEFFilterDmpaingFactorMinus3 = cdef->cdef_damping_minus_3;
      fil.CDEFYStrength4 = cdef_strengths[4];
      fil.CDEFYStrength5 = cdef_strengths[5];
      fil.CDEFYStrength6 = cdef_strengths[6];
      fil.CDEFYStrength7 = cdef_strengths[7];
      fil.CDEFUVStrength0 = cdef_uv_strengths[0];
      fil.CDEFUVStrength1 = cdef_uv_strengths[1];
      fil.CDEFUVStrength2 = cdef_uv_strengths[2];
      fil.CDEFUVStrength3 = cdef_uv_strengths[3];
      fil.CDEFUVStrength4 = cdef_uv_strengths[4];
      fil.CDEFUVStrength5 = cdef_uv_strengths[5];
      fil.CDEFUVStrength6 = cdef_uv_strengths[6];
      fil.CDEFUVStrength7 = cdef_uv_strengths[7];
      fil.SuperResUpscaledFrameWidthMinus1 = av1_pic_info->frame_header->frame_width_minus_1;
      fil.SuperResDenom = av1_pic_info->frame_header->flags.use_superres ? 0/*TODO*/ : 8;
      fil.FrameLoopRestorationFilterLumaY = frame_restoration_type[0];
      fil.FrameLoopRestorationFilterChromaU = frame_restoration_type[1];
      fil.FrameLoopRestorationFilterChromaV = frame_restoration_type[2];
      fil.LoopRestorationUnitSizeLumaY = (!frame_restoration_type[0] && !frame_restoration_type[1] && !frame_restoration_type[2]) ? 0 : av1_pic_info->frame_header->lr.lr_unit_shift + 1;
      fil.UseSameLoopRestorationUnitSizeChromasUVFlag = ((frame_restoration_type[1] || frame_restoration_type[2]) && av1_pic_info->frame_header->lr.lr_uv_shift == 0) ? 1 : 0;
      fil.LumaPlanex_step_qn = 0;
      fil.LumaPlanex0_qn = 0;
      fil.ChromaPlanex_step_qn = 0;
      fil.ChromaPlanex0_qn = 0;
   };

   struct StdVideoDecodeAV1MESATile *cur_tile = &av1_pic_info->tile_list->tile_list[tile_idx];
   anv_batch_emit(&cmd_buffer->batch, GENX(AVP_TILE_CODING), til) {
      til.FrameTileID = tile_idx;
      til.TGTileNum = tile_idx;
      til.TileGroupID = 0;
      til.TileColumnPositioninSBUnit = cur_tile->column;
      til.TileRowPositioninSBUnit = cur_tile->row;
      til.TileWidthinSBMinus1 = av1_pic_info->frame_header->tiling.width_in_sbs_minus_1[cur_tile->column];
      til.TileHeightinSBMinus1 = av1_pic_info->frame_header->tiling.height_in_sbs_minus_1[cur_tile->row];
      til.IsLastTileofRowFlag = cur_tile->column == av1_pic_info->frame_header->tiling.tile_cols - 1;
      til.IsLastTileofColumnFlag = cur_tile->row == av1_pic_info->frame_header->tiling.tile_rows - 1;
      til.IsStartTileofTileGroupFlag = (tile_idx == cur_tile->tg_start);
      til.IsEndTileofTileGroupFlag = (tile_idx == cur_tile->tg_end);
      til.IsLastTileofFrameFlag = (cur_tile->column == av1_pic_info->frame_header->tiling.tile_cols - 1) &&
         (cur_tile->row == av1_pic_info->frame_header->tiling.tile_rows - 1);
      til.DisableCDFUpdateFlag = av1_pic_info->frame_header->flags.disable_cdf_update;
      til.DisableFrameContextUpdateFlag = av1_pic_info->frame_header->flags.disable_frame_end_update_cdf || (tile_idx != av1_pic_info->frame_header->tiling.context_update_tile_id);
      til.NumberofActiveBEPipes = 1;
      til.NumofTileColumnsinFrameMinus1 = av1_pic_info->frame_header->tiling.tile_cols - 1;
      til.NumofTileRowsinFrameMinus1 = av1_pic_info->frame_header->tiling.tile_rows - 1;
   };

   anv_batch_emit(&cmd_buffer->batch, GENX(AVP_BSD_OBJECT), bsd) {
      bsd.TileIndirectBSDDataLength = cur_tile->size;
      bsd.TileIndirectDataStartAddress = cur_tile->offset;
   };

   anv_batch_emit(&cmd_buffer->batch, GENX(AVP_VD_CONTROL_STATE), vd) {
      vd.VDControlState.MemoryImplicitFlush = 1;
   }
   anv_batch_emit(&cmd_buffer->batch, GENX(VD_PIPELINE_FLUSH), vd) {
      vd.AVPPipelineDone = 1;
      vd.VDCommandMessageParserDone = 1;
      vd.AVPPipelineCommandFlush = 1;
   }
}

static void
anv_av1_decode_video(struct anv_cmd_buffer *cmd_buffer,
                     const VkVideoDecodeInfoKHR *frame_info)
{
   const struct VkVideoDecodeAV1PictureInfoMESA *av1_pic_info =
      vk_find_struct_const(frame_info->pNext, VIDEO_DECODE_AV1_PICTURE_INFO_MESA);
   for (unsigned t = 0; t < av1_pic_info->tile_list->nb_tiles; t++)
      anv_av1_decode_video_tile(cmd_buffer, frame_info, t);
}
#endif

void
genX(CmdDecodeVideoKHR)(VkCommandBuffer commandBuffer,
                        const VkVideoDecodeInfoKHR *frame_info)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   switch (cmd_buffer->video.vid->vk.op) {
   case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR:
      anv_h264_decode_video(cmd_buffer, frame_info);
      break;
#if GFX_VERx10 >= 120
   case VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_MESA:
      anv_av1_decode_video(cmd_buffer, frame_info);
      break;
#endif
   default:
      assert(0);
   }
}

#ifdef VK_ENABLE_BETA_EXTENSIONS
void
genX(CmdEncodeVideoKHR)(VkCommandBuffer commandBuffer,
                        const VkVideoEncodeInfoKHR *pEncodeInfo)
{
}
#endif
