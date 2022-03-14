/**************************************************************************
 *
 * Copyright 2017 Advanced Micro Devices, Inc.
 * Copyright 2021 Red Hat Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/
#include "radv_private.h"

#include "ac_vcn_dec.h"
#include "ac_uvd_dec.h"

void
radv_init_physical_device_decoder(struct radv_physical_device *pdevice)
{
   switch (pdevice->rad_info.family) {
   case CHIP_VEGA10:
   case CHIP_VEGA12:
   case CHIP_VEGA20:
      pdevice->vid_dec_reg.data0 = RUVD_GPCOM_VCPU_DATA0_SOC15;
      pdevice->vid_dec_reg.data1 = RUVD_GPCOM_VCPU_DATA1_SOC15;
      pdevice->vid_dec_reg.cmd = RUVD_GPCOM_VCPU_CMD_SOC15;
      pdevice->vid_dec_reg.cntl = RUVD_ENGINE_CNTL_SOC15;
      break;
   case CHIP_RAVEN:
   case CHIP_RAVEN2:
      pdevice->vid_dec_reg.data0 = RDECODE_VCN1_GPCOM_VCPU_DATA0;
      pdevice->vid_dec_reg.data1 = RDECODE_VCN1_GPCOM_VCPU_DATA1;
      pdevice->vid_dec_reg.cmd = RDECODE_VCN1_GPCOM_VCPU_CMD;
      pdevice->vid_dec_reg.cntl = RDECODE_VCN1_ENGINE_CNTL;
      break;
   case CHIP_NAVI10:
   case CHIP_NAVI12:
   case CHIP_NAVI14:
   case CHIP_RENOIR:
      pdevice->vid_dec_reg.data0 = RDECODE_VCN2_GPCOM_VCPU_DATA0;
      pdevice->vid_dec_reg.data1 = RDECODE_VCN2_GPCOM_VCPU_DATA1;
      pdevice->vid_dec_reg.cmd = RDECODE_VCN2_GPCOM_VCPU_CMD;
      pdevice->vid_dec_reg.cntl = RDECODE_VCN2_ENGINE_CNTL;
      break;
   case CHIP_MI100:
   case CHIP_MI200:
   case CHIP_NAVI21:
   case CHIP_NAVI22:
   case CHIP_NAVI23:
   case CHIP_NAVI24:
   case CHIP_VANGOGH:
   case CHIP_REMBRANDT:
      pdevice->vid_dec_reg.data0 = RDECODE_VCN2_5_GPCOM_VCPU_DATA0;
      pdevice->vid_dec_reg.data1 = RDECODE_VCN2_5_GPCOM_VCPU_DATA1;
      pdevice->vid_dec_reg.cmd = RDECODE_VCN2_5_GPCOM_VCPU_CMD;
      pdevice->vid_dec_reg.cntl = RDECODE_VCN2_5_ENGINE_CNTL;
      break;
   default:
      if (radv_has_uvd(pdevice)) {
         pdevice->vid_dec_reg.data0 = RUVD_GPCOM_VCPU_DATA0;
         pdevice->vid_dec_reg.data1 = RUVD_GPCOM_VCPU_DATA1;
         pdevice->vid_dec_reg.cmd = RUVD_GPCOM_VCPU_CMD;
         pdevice->vid_dec_reg.cntl = RUVD_ENGINE_CNTL;
      }
      break;
   }
}
