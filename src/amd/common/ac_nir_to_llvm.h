/*
 * Copyright © 2016 Bas Nieuwenhuizen
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

#pragma once

#include "llvm-c/Core.h"
#include "llvm-c/TargetMachine.h"

struct ac_shader_binary;
struct ac_shader_config;
struct nir_shader;
struct radv_pipeline_layout;

struct ac_nir_compiler_options {
	struct radv_pipeline_layout *layout;
};

struct ac_shader_variant_info {
	unsigned num_user_sgprs;
	unsigned num_input_sgprs;
	unsigned num_input_vgprs;
	union {
		struct {
			unsigned param_exports;
		} vs;
		struct {
			unsigned num_interp;
			uint32_t flat_shaded_mask;
			uint32_t spi_mapping[32];
		} fs;
	};
};

void ac_compile_nir_shader(LLVMTargetMachineRef tm,
                           struct ac_shader_binary *binary,
                           struct ac_shader_config *config,
                           struct ac_shader_variant_info *shader_info,
                           struct nir_shader *nir,
                           const struct ac_nir_compiler_options *options);
