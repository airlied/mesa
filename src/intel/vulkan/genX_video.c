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

#ifdef VK_ENABLE_BETA_EXTENSIONS
void
genX(CmdBeginVideoCodingKHR)(VkCommandBuffer commandBuffer,
                             const VkVideoBeginCodingInfoKHR *pBeginInfo)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_video_session, vid, pBeginInfo->videoSession);
   ANV_FROM_HANDLE(anv_video_session_params, params, pBeginInfo->videoSessionParameters);

   cmd_buffer->video.vid = vid;
   cmd_buffer->video.params = params;
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

static uint32_t avc_get_first_mb_bit_offset(struct vk_video_h264_slice_params *slice_params,
                                            const StdVideoH264PictureParameterSet *pps)
{
    unsigned int slice_data_bit_offset = slice_params->slice_data_bit_offset;

    if (pps->flags.entropy_coding_mode_flag)
        slice_data_bit_offset = ALIGN(slice_data_bit_offset, 0x8);
    return slice_data_bit_offset;
}

static void avc_fill_weight_offset_table(struct vk_video_h264_slice_params *slice_params, int index, int16_t *offsets)
{
# define getter(name) (index == 0 ? slice_params->name##_l0 : slice_params->name##_l1)

   /* The packet has 96 32 bit values in it, we have that data as 192 16 bit
    * values. So, we pack that into an array of 192 16 bit values, then memcpy
    * it into the 32 bit array the hardware wants.
    */
   for (uint32_t j = 0; j < 32; j++) {
      offsets[j * 6    ] = getter(luma_weight)[j];
      offsets[j * 6 + 1] = getter(luma_offset)[j];
      offsets[j * 6 + 2] = getter(chroma_weight)[j][0];
      offsets[j * 6 + 3] = getter(chroma_offset)[j][0];
      offsets[j * 6 + 4] = getter(chroma_weight)[j][1];
      offsets[j * 6 + 5] = getter(chroma_offset)[j][1];
   }
#undef getter
}

static inline void
set_avc_ref_idx_reference_list(const VkVideoDecodeInfoKHR *frame_info,
                               const struct vk_video_h264_reference *ref_slots,
                               struct GENX(MFX_AVC_REF_IDX_STATE) *avc_ref_idx,
                               uint count, int32_t *sorted_idx)
{
   unsigned i = 0;
   for(i = 0; i < count; i++) {
      if (i >= frame_info->referenceSlotCount) {
         avc_ref_idx->ReferenceListEntry[i] = 0xff;
      } else {
         /* Shameless lifted from intel-vaapi
          *
          * The H.264 standard, and the VA-API specification, allows for at
          * least 3 states for a picture: "used for short-term reference",
          * "used for long-term reference", or considered as not used for
          * reference.
          *
          * The latter is used in the MVC inter prediction and inter-view
          * prediction process (H.8.4). This has an incidence on the
          * colZeroFlag variable, as defined in 8.4.1.2.
          *
          * Since it is not possible to directly program that flag, let's
          * make the hardware derive this value by assimilating "considered
          * as not used for reference" to a "not used for short-term
          * reference", and subsequently making it "used for long-term
          * reference" to fit the definition of Bit6 here
          */
         int idx = sorted_idx ? sorted_idx[i] : i;
         const struct vk_video_h264_reference *ref_info = &ref_slots[idx];
         fprintf(stderr, "mapping %d %d to %d\n", i, idx, ref_info->slot_index);

         avc_ref_idx->ReferenceListEntry[i] = (
            (ref_info->flags.is_long_term << 6) |
            ((ref_info->flags.top_field_flag ^ ref_info->flags.bottom_field_flag ^ 1) << 5) |
            (ref_info->slot_index << 1) |
            ((ref_info->flags.top_field_flag ^ 1) & ref_info->flags.bottom_field_flag)
         );
      }
   }
   for (; i < 32; i++) {
      avc_ref_idx->ReferenceListEntry[i] = 0xff;
   }
}

static void
anv_h264_decode_video(struct anv_cmd_buffer *cmd_buffer,
                      const VkVideoDecodeInfoKHR *frame_info)
{

   ANV_FROM_HANDLE(anv_buffer, src_buffer, frame_info->srcBuffer);
   struct anv_video_session *vid = cmd_buffer->video.vid;
   struct anv_video_session_params *params = cmd_buffer->video.params;

   // H264 only so far
   struct vk_video_h264_slice_params slice_params = { 0 };

   const struct VkVideoDecodeH264PictureInfoEXT *h264_pic_info =
      vk_find_struct_const(frame_info->pNext, VIDEO_DECODE_H264_PICTURE_INFO_EXT);
   const StdVideoH264SequenceParameterSet *sps = &params->vk.h264_dec.sps_std[h264_pic_info->pStdPictureInfo->seq_parameter_set_id];
   const StdVideoH264PictureParameterSet *pps = &params->vk.h264_dec.pps_std[h264_pic_info->pStdPictureInfo->pic_parameter_set_id];
   struct vk_video_h264_reference ref_slots[32];

   vk_fill_video_reference_info(frame_info, ref_slots);

   void *slice_map = anv_gem_mmap(cmd_buffer->device, src_buffer->address.bo->gem_handle,
                                  src_buffer->address.offset, frame_info->srcBufferRange, 0);
   vk_video_parse_h264_slice_header(frame_info, sps, pps, slice_map, &slice_params);
   anv_gem_munmap(cmd_buffer->device, slice_map, frame_info->srcBufferRange);

   anv_batch_emit(&cmd_buffer->batch, GENX(MI_FLUSH_DW), flush) {
      flush.DWordLength = 2;
      flush.VideoPipelineCacheInvalidate = 1;
   };

   anv_batch_emit(&cmd_buffer->batch, GENX(MFX_PIPE_MODE_SELECT), sel) {
      sel.StandardSelect = SS_AVC;
      sel.CodecSelect = Decode;
      sel.DecoderShortFormatMode = LongFormatDriverInterface; // VAAPI driver says "Currently only support long format"
      sel.DecoderModeSelect = VLDMode; // Hardcoded

      bool enable_avc_ildb = slice_params.disable_deblocking_filter_idc != 1;
      sel.PreDeblockingOutputEnable = !enable_avc_ildb;
      sel.PostDeblockingOutputEnable = enable_avc_ildb;
   }

   const struct anv_image_view *iv = anv_image_view_from_handle(frame_info->dstPictureResource.imageViewBinding);
   const struct anv_image *img = iv->image;
   anv_batch_emit(&cmd_buffer->batch, GENX(MFX_SURFACE_STATE), ss) {
      ss.Width = frame_info->dstPictureResource.codedExtent.width - 1;
      ss.Height = frame_info->dstPictureResource.codedExtent.height - 1;
      ss.SurfaceFormat = PLANAR_420_8; // assert on this?
      ss.InterleaveChroma = 1;
      ss.SurfacePitch = img->planes[0].primary_surface.isl.row_pitch_B - 1;
      ss.TiledSurface = img->planes[0].primary_surface.isl.tiling != ISL_TILING_LINEAR;
      assert (img->planes[0].primary_surface.isl.tiling == ISL_TILING_Y0);
      ss.TileWalk = TW_YMAJOR;

      ss.YOffsetforUCb = align(frame_info->dstPictureResource.codedExtent.height, 32);
   }

   anv_batch_emit(&cmd_buffer->batch, GENX(MFX_PIPE_BUF_ADDR_STATE), buf) {
      if (slice_params.disable_deblocking_filter_idc == 1) {
         buf.PreDeblockingDestinationAddress = anv_image_address(img,
                                                                 &img->planes[0].primary_surface.memory_range);
      } else {
         buf.PostDeblockingDestinationAddress = anv_image_address(img,
                                                                  &img->planes[0].primary_surface.memory_range);
      }
#if GFX_VERx10 >= 75 && GFX_VER < 9
      buf.PreDeblockingDestinationMOCS = anv_mocs(cmd_buffer->device, buf.PreDeblockingDestinationAddress.bo, 0);
      buf.PostDeblockingDestinationMOCS = anv_mocs(cmd_buffer->device, buf.PostDeblockingDestinationAddress.bo, 0);
      buf.OriginalUncompressedPictureSourceMOCS = anv_mocs(cmd_buffer->device, NULL, 0);
      buf.StreamOutDataDestinationMOCS = anv_mocs(cmd_buffer->device, NULL, 0);
#endif


#if GFX_VER == 8
      buf.IntraRowStoreScratchBufferAddressHigh = (struct anv_address) { vid->h264.intra_row_scratch.mem->bo,
         vid->h264.intra_row_scratch.offset };
      buf.IntraRowStoreScratchBufferMOCS = anv_mocs(cmd_buffer->device, vid->h264.intra_row_scratch.mem->bo, 0);
      buf.DeblockingFilterRowStoreScratchAddressHigh = (struct anv_address) { vid->h264.deblocking_filter_row_scratch.mem->bo,
         vid->h264.deblocking_filter_row_scratch.offset };
#else
      buf.IntraRowStoreScratchBufferAddress = (struct anv_address) { vid->h264.intra_row_scratch.mem->bo, vid->h264.intra_row_scratch.offset };
#if GFX_VERx10 >= 75 && GFX_VER < 9
      buf.IntraRowStoreScratchBufferMOCS = anv_mocs(cmd_buffer->device, vid->h264.intra_row_scratch.mem->bo, 0);
#endif
#if GFX_VERx10 == 70
      buf.DeblockingFilterRowStoreScratchBufferAddress = (struct anv_address) { vid->h264.deblocking_filter_row_scratch.mem->bo, vid->h264.deblocking_filter_row_scratch.offset };
#else
      buf.DeblockingFilterRowStoreScratchAddress = (struct anv_address) { vid->h264.deblocking_filter_row_scratch.mem->bo, vid->h264.deblocking_filter_row_scratch.offset };
#endif
#endif
#if GFX_VERx10 >= 75 && GFX_VER < 8
      buf.DeblockingFilterRowStoreScratchMOCS = anv_mocs(cmd_buffer->device, vid->h264.deblocking_filter_row_scratch.mem->bo, 0);
      buf.MBStatusBufferMOCS = anv_mocs(cmd_buffer->device, NULL, 0);
      buf.MBILDBStreamOutBufferMOCS = anv_mocs(cmd_buffer->device, NULL, 0);
#endif

      for (unsigned i = 0; i < frame_info->referenceSlotCount; i++) {
         const struct anv_image_view *ref_iv = anv_image_view_from_handle(ref_slots[i].pPictureResource->imageViewBinding);
         int idx = ref_slots[i].slot_index;
         buf.ReferencePictureAddress[idx] = anv_image_address(ref_iv->image,
                                                            &ref_iv->image->planes[0].primary_surface.memory_range);
      }
   }

   anv_batch_emit(&cmd_buffer->batch, GENX(MFX_BSP_BUF_BASE_ADDR_STATE), bsp) {
      bsp.BSDMPCRowStoreScratchBufferAddress = (struct anv_address) { vid->h264.bsd_mpc_row_scratch.mem->bo,
         vid->h264.bsd_mpc_row_scratch.offset };
#if GFX_VERx10 == 75
      bsp.BSDMPCRowStoreScratchBufferMOCS = anv_mocs(cmd_buffer->device, vid->h264.bsd_mpc_row_scratch.mem->bo, 0);
#endif

      bsp.MPRRowStoreScratchBufferAddress = (struct anv_address) { vid->h264.mpr_row_store_scratch.mem->bo,
         vid->h264.mpr_row_store_scratch.offset };

#if GFX_VERx10 == 75
      bsp.MPRRowStoreScratchBufferMOCS = anv_mocs(cmd_buffer->device, vid->h264.mpr_row_store_scratch.mem->bo, 0);
      bsp.BitplaneReadBufferMOCS = anv_mocs(cmd_buffer->device, NULL, 0);
#endif
   }

   if (pps->flags.pic_scaling_matrix_present_flag) {
      anv_batch_emit(&cmd_buffer->batch, GENX(MFX_QM_STATE), qm) {
         qm.DWordLength = 16;
         qm.AVC = AVC_4x4_Intra_MATRIX;
      }
      anv_batch_emit(&cmd_buffer->batch, GENX(MFX_QM_STATE), qm) {
         qm.DWordLength = 16;
         qm.AVC = AVC_4x4_Inter_MATRIX;
      }
      if (pps->flags.transform_8x8_mode_flag) {
         anv_batch_emit(&cmd_buffer->batch, GENX(MFX_QM_STATE), qm) {
            qm.DWordLength = 16;
            qm.AVC = AVC_8x8_Intra_MATRIX;
         }
         anv_batch_emit(&cmd_buffer->batch, GENX(MFX_QM_STATE), qm) {
            qm.DWordLength = 16;
            qm.AVC = AVC_8x8_Inter_MATRIX;
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


#if GFX_VERx10 >= 75
   anv_batch_emit(&cmd_buffer->batch, GENX(MFD_AVC_PICID_STATE), picid) {
      picid.PictureIDRemappingDisable = 1;
   }
#endif
   unsigned w_mb = align(img->vk.extent.width, ANV_MB_WIDTH) / ANV_MB_WIDTH;
   unsigned h_mb = align(img->vk.extent.height, ANV_MB_HEIGHT) / ANV_MB_HEIGHT;

   anv_batch_emit(&cmd_buffer->batch, GENX(MFX_AVC_IMG_STATE), avc_img) {
      avc_img.FrameSize = (w_mb * h_mb) - 1;
      avc_img.FrameWidth = w_mb - 1;
      avc_img.FrameHeight = h_mb - 1;

      if (!h264_pic_info->pStdPictureInfo->flags.field_pic_flag)
         avc_img.ImageStructure = FramePicture;
      else if (h264_pic_info->pStdPictureInfo->flags.bottom_field_flag)
         avc_img.ImageStructure = BottomFieldPicture;
      else
         avc_img.ImageStructure = TopFieldPicture;

      avc_img.WeightedBiPredictionIDC = pps->weighted_bipred_idc;
      avc_img.WeightedPredictionEnable = pps->flags.weighted_pred_flag;
      avc_img.FirstChromaQPOffset = pps->chroma_qp_index_offset & 0x1f;
      avc_img.SecondChromaQPOffset = pps->second_chroma_qp_index_offset & 0x1f;
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
   }

   anv_batch_emit(&cmd_buffer->batch, GENX(MFX_IND_OBJ_BASE_ADDR_STATE), index_obj) {
      index_obj.MFXIndirectBitstreamObjectAddress = anv_address_add(src_buffer->address,
                                                                    frame_info->srcBufferOffset);
#if GFX_VERx10 == 75
      index_obj.MFXIndirectBitstreamObjectMOCS = anv_mocs(cmd_buffer->device, src_buffer->address.bo,
                                                          0);
      index_obj.MFXIndirectMVObjectMOCS = anv_mocs(cmd_buffer->device, NULL, 0);
      index_obj.MFDIndirectITCOEFFObjectMOCS = anv_mocs(cmd_buffer->device, NULL, 0);
      index_obj.MFDIndirectITDBLKObjectMOCS = anv_mocs(cmd_buffer->device, NULL, 0);
      index_obj.MFCIndirectPAKBSEObjectMOCS = anv_mocs(cmd_buffer->device, NULL, 0);
#endif
#if GFX_VER == 7
      index_obj.MFXIndirectBitstreamObjectAccessUpperBound = (struct anv_address) { NULL, 0x80000000 };
#endif
   }

   anv_batch_emit(&cmd_buffer->batch, GENX(MFX_AVC_DIRECTMODE_STATE), avc_directmode) {
      /* bind reference frame DMV */
      for (unsigned i = 0; i < frame_info->referenceSlotCount; i++) {
         int idx = ref_slots[i].slot_index;
         const struct anv_image_view *ref_iv = anv_image_view_from_handle(ref_slots[i].pPictureResource->imageViewBinding);
         avc_directmode.DirectMVBufferAddress[idx] = anv_image_address(ref_iv->image,
                                                                     &ref_iv->image->vid_dmv_top_surface);
         avc_directmode.POCList[2 * idx] = ref_slots[i].pic_order_cnt[0];
         avc_directmode.POCList[2 * idx + 1] = ref_slots[i].pic_order_cnt[1];
      }
#if GFX_VERx10 == 70
      avc_directmode.DirectMVBufferWriteAddress[0] = anv_image_address(img,
                                                                       &img->vid_dmv_top_surface);
#else
      avc_directmode.DirectMVBufferWriteAddress = anv_image_address(img,
                                                                    &img->vid_dmv_top_surface);
#endif
      avc_directmode.POCList[32] = h264_pic_info->pStdPictureInfo->PicOrderCnt[0];
      avc_directmode.POCList[33] = h264_pic_info->pStdPictureInfo->PicOrderCnt[1];
   }

   if (slice_params.slice_type == STD_VIDEO_H264_SLICE_TYPE_B) {
      /* the B frame lists have to be sorted specially. */
      int32_t sorted_l0_idxs[32], sorted_l1_idxs[32];
      uint32_t curr_poc = h264_pic_info->pStdPictureInfo->PicOrderCnt[0];

      vk_video_sort_b_l0_ref_frames(frame_info->referenceSlotCount,
                                    curr_poc,
                                    ref_slots,
                                    sorted_l0_idxs);
      vk_video_sort_b_l1_ref_frames(frame_info->referenceSlotCount,
                                    curr_poc,
                                    ref_slots,
                                    sorted_l1_idxs);

      anv_batch_emit(&cmd_buffer->batch, GENX(MFX_AVC_REF_IDX_STATE), avc_ref_idx) {
         set_avc_ref_idx_reference_list(frame_info, ref_slots, &avc_ref_idx, slice_params.num_ref_idx_l0_active_minus1 + 1, sorted_l0_idxs);
      }
      anv_batch_emit(&cmd_buffer->batch, GENX(MFX_AVC_REF_IDX_STATE), avc_ref_idx) {
         avc_ref_idx.ReferencePictureListSelect = 1;
         set_avc_ref_idx_reference_list(frame_info, ref_slots, &avc_ref_idx, slice_params.num_ref_idx_l1_active_minus1 + 1, sorted_l1_idxs);
      }
   } else if (slice_params.slice_type == STD_VIDEO_H264_SLICE_TYPE_P) {
      int32_t sorted_p_idxs[32];
      vk_video_sort_p_ref_frames(frame_info->referenceSlotCount,
                                 ref_slots,
                                 &slice_params,
                                 h264_pic_info->pStdPictureInfo->frame_num,
                                 sorted_p_idxs);

      anv_batch_emit(&cmd_buffer->batch, GENX(MFX_AVC_REF_IDX_STATE), avc_ref_idx) {
         set_avc_ref_idx_reference_list(frame_info, ref_slots, &avc_ref_idx, slice_params.num_ref_idx_l0_active_minus1 + 1, sorted_p_idxs);
      }
   }

   if (pps->flags.weighted_pred_flag) {
      if (slice_params.slice_type == STD_VIDEO_H264_SLICE_TYPE_P ||
          slice_params.slice_type == STD_VIDEO_H264_SLICE_TYPE_B) {
         anv_batch_emit(&cmd_buffer->batch, GENX(MFX_AVC_WEIGHTOFFSET_STATE), weight) {
            weight.WeightandOffsetSelect = 0;
            avc_fill_weight_offset_table(&slice_params, 0, (int16_t *)&weight.WeightOffset);
         }
      }

      if (slice_params.slice_type == STD_VIDEO_H264_SLICE_TYPE_B) {
         anv_batch_emit(&cmd_buffer->batch, GENX(MFX_AVC_WEIGHTOFFSET_STATE), weight) {
            weight.WeightandOffsetSelect = 1;
            avc_fill_weight_offset_table(&slice_params, 1, (int16_t *)&weight.WeightOffset);
         }
      }
   }

   anv_batch_emit(&cmd_buffer->batch, GENX(MFX_AVC_SLICE_STATE), avc_slice) {
      avc_slice.SliceType = slice_params.slice_type;
      avc_slice.Log2WeightDenominatorLuma = slice_params.luma_log2_weight_denom;
      avc_slice.Log2WeightDenominatorChroma = slice_params.chroma_log2_weight_denom;
      avc_slice.NumberofReferencePicturesinInterpredictionList0 =
         slice_params.slice_type == STD_VIDEO_H264_SLICE_TYPE_I ? 0 : slice_params.num_ref_idx_l0_active_minus1 + 1;
      avc_slice.NumberofReferencePicturesinInterpredictionList1 =
         (slice_params.slice_type == STD_VIDEO_H264_SLICE_TYPE_I ||
          slice_params.slice_type == STD_VIDEO_H264_SLICE_TYPE_P) ? 0 : slice_params.num_ref_idx_l1_active_minus1 + 1;
      avc_slice.SliceAlphaC0OffsetDiv2 = slice_params.slice_alpha_c0_offset_div2;
      avc_slice.SliceBetaOffsetDiv2 = slice_params.slice_beta_offset_div2;
      avc_slice.SliceQuantizationParameter =
         pps->pic_init_qp_minus26 + 26 + slice_params.slice_qp_delta;
      avc_slice.CABACInitIDC = slice_params.cabac_init_idc;
      avc_slice.DisableDeblockingFilterIndicator = slice_params.disable_deblocking_filter_idc;
      avc_slice.DirectPredictionType = slice_params.direct_spatial_mv_pred_flag;

      // In Intel VAAPI this is conditonal, but that handles multiple slices, which we don't

      avc_slice.SliceStartMBNumber = slice_params.first_mb_in_slice;
      avc_slice.SliceHorizontalPosition =
         slice_params.first_mb_in_slice % (w_mb);
      avc_slice.SliceVerticalPosition =
         slice_params.first_mb_in_slice / (w_mb);
      if (!h264_pic_info->pStdPictureInfo->flags.field_pic_flag && sps->flags.mb_adaptive_frame_field_flag) {
         avc_slice.SliceVerticalPosition <<= 1;
      }
      avc_slice.NextSliceHorizontalPosition = 0;
      avc_slice.NextSliceVerticalPosition = (h_mb / (1 + !!h264_pic_info->pStdPictureInfo->flags.field_pic_flag));
      avc_slice.LastSliceGroup = 1;
   }

   unsigned slice_data_bit_offset = avc_get_first_mb_bit_offset(&slice_params, pps);
   anv_batch_emit(&cmd_buffer->batch, GENX(MFD_AVC_BSD_OBJECT), avc_bsd) {
      avc_bsd.IndirectBSDDataLength = frame_info->srcBufferRange;
      /* start decoding after the 3-byte header. */
      avc_bsd.IndirectBSDDataStartAddress = (struct anv_address) { NULL, 3 };
      avc_bsd.InlineData.FirstMBBitOffset = slice_data_bit_offset & 0x7;
      avc_bsd.InlineData.LastSlice = 1;
      avc_bsd.InlineData.FixPrevMBSkipped = 1;
      avc_bsd.InlineData.FirstMBByteOffsetofSliceDataorSliceHeader = (slice_data_bit_offset >> 3);
    }
}

void
genX(CmdDecodeVideoKHR)(VkCommandBuffer commandBuffer,
                        const VkVideoDecodeInfoKHR *frame_info)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   switch (cmd_buffer->video.vid->vk.op) {
   case VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_EXT:
      anv_h264_decode_video(cmd_buffer, frame_info);
      break;
   default:
      assert(0);
   }
}

void
genX(CmdEncodeVideoKHR)(VkCommandBuffer commandBuffer,
                        const VkVideoEncodeInfoKHR *pEncodeInfo)
{
}
#endif
