/*
 * Copyright © 2016 Red Hat.
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
#ifndef SPIRV_AMD_H
#define SPIRV_AMD_H

enum AMDSPVGCNShader {
  SpvOpCubeFaceIndexAMD = 1,
  SpvOpCubeFaceCoordAMD = 2,
  SpvOpTimeAMD = 3,
};

enum AMDSPVTrinaryMinmax {
  SpvOpFMin3AMD = 1,
  SpvOpUMin3AMD,
  SpvOpSMin3AMD,
  SpvOpFMax3AMD,
  SpvOpUMax3AMD,
  SpvOpSMax3AMD,
  SpvOpFMid3AMD,
  SpvOpUMid3AMD,
  SpvOpSMid3AMD,
};

#endif
