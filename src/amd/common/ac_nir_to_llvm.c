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

#include "ac_nir_to_llvm.h"
#include "ac_binary.h"
#include "sid.h"
#include "nir/nir.h"
#include "../vulkan/radv_descriptor_set.h"
#include "util/bitscan.h"
#include <llvm-c/Transforms/Scalar.h>

enum radeon_llvm_calling_convention {
	RADEON_LLVM_AMDGPU_VS = 87,
	RADEON_LLVM_AMDGPU_GS = 88,
	RADEON_LLVM_AMDGPU_PS = 89,
	RADEON_LLVM_AMDGPU_CS = 90,
};

#define CONST_ADDR_SPACE 2
#define LOCAL_ADDR_SPACE 3

#define RADEON_LLVM_MAX_INPUTS (VARYING_SLOT_VAR31 + 1)
#define RADEON_LLVM_MAX_OUTPUTS (VARYING_SLOT_VAR31 + 1)

enum desc_type {
	DESC_IMAGE,
	DESC_FMASK,
	DESC_SAMPLER,
	DESC_BUFFER,
};

struct nir_to_llvm_context {
	const struct ac_nir_compiler_options *options;
	struct ac_shader_variant_info *shader_info;

	LLVMContextRef context;
	LLVMModuleRef module;
	LLVMBuilderRef builder;
	LLVMValueRef main_function;

	struct hash_table *defs;
	struct hash_table *phis;

	LLVMValueRef descriptor_sets[4];
	LLVMValueRef push_constants;
	LLVMValueRef num_work_groups;
	LLVMValueRef workgroup_ids;
	LLVMValueRef local_invocation_ids;

	LLVMValueRef vertex_buffers;
	LLVMValueRef base_vertex;
	LLVMValueRef start_instance;
	LLVMValueRef vertex_id;
	LLVMValueRef rel_auto_id;
	LLVMValueRef vs_prim_id;
	LLVMValueRef instance_id;

	LLVMValueRef prim_mask;
	LLVMValueRef persp_sample, persp_center, persp_centroid;
	LLVMValueRef linear_sample, linear_center, linear_centroid;

	LLVMBasicBlockRef continue_block;
	LLVMBasicBlockRef break_block;

	LLVMTypeRef i1;
	LLVMTypeRef i8;
	LLVMTypeRef i16;
	LLVMTypeRef i32;
	LLVMTypeRef v2i32;
	LLVMTypeRef v3i32;
	LLVMTypeRef v4i32;
	LLVMTypeRef v8i32;
	LLVMTypeRef f32;
	LLVMTypeRef v4f32;
	LLVMTypeRef v16i8;
	LLVMTypeRef voidt;

	LLVMValueRef i32zero;
	LLVMValueRef i32one;
	LLVMValueRef f32zero;
	LLVMValueRef f32one;
	LLVMValueRef v4f32empty;

	unsigned uniform_md_kind;
	LLVMValueRef empty_md;
	LLVMValueRef const_md;
	gl_shader_stage stage;

	LLVMValueRef inputs[RADEON_LLVM_MAX_INPUTS * 4];
	LLVMValueRef outputs[RADEON_LLVM_MAX_OUTPUTS * 4];
	uint64_t input_mask;
	uint64_t output_mask;
	int num_locals;
	LLVMValueRef *locals;
};

struct ac_tex_info {
	LLVMValueRef args[12];
	int arg_count;
	LLVMTypeRef dst_type;
};

static LLVMValueRef
emit_llvm_intrinsic(struct nir_to_llvm_context *ctx, const char *name,
                    LLVMTypeRef return_type, LLVMValueRef *params,
                    unsigned param_count, LLVMAttribute attribs);
static LLVMValueRef get_sampler_desc(struct nir_to_llvm_context *ctx,
				     nir_deref_var *deref,
				     LLVMValueRef index,
				     enum desc_type desc_type);
static unsigned radeon_llvm_reg_index_soa(unsigned index, unsigned chan)
{
	return (index * 4) + chan;
}

static unsigned llvm_get_type_size(LLVMTypeRef type)
{
	LLVMTypeKind kind = LLVMGetTypeKind(type);

	switch (kind) {
	case LLVMIntegerTypeKind:
		return LLVMGetIntTypeWidth(type) / 8;
	case LLVMFloatTypeKind:
		return 4;
	case LLVMPointerTypeKind:
		return 8;
	case LLVMVectorTypeKind:
		return LLVMGetVectorSize(type) *
		       llvm_get_type_size(LLVMGetElementType(type));
	default:
		assert(0);
		return 0;
	}
}

static void set_llvm_calling_convention(LLVMValueRef func,
                                        gl_shader_stage stage)
{
	enum radeon_llvm_calling_convention calling_conv;

	switch (stage) {
	case MESA_SHADER_VERTEX:
	case MESA_SHADER_TESS_CTRL:
	case MESA_SHADER_TESS_EVAL:
		calling_conv = RADEON_LLVM_AMDGPU_VS;
		break;
	case MESA_SHADER_GEOMETRY:
		calling_conv = RADEON_LLVM_AMDGPU_GS;
		break;
	case MESA_SHADER_FRAGMENT:
		calling_conv = RADEON_LLVM_AMDGPU_PS;
		break;
	case MESA_SHADER_COMPUTE:
		calling_conv = RADEON_LLVM_AMDGPU_CS;
		break;
	default:
		unreachable("Unhandle shader type");
	}

	LLVMSetFunctionCallConv(func, calling_conv);
}

static LLVMValueRef
create_llvm_function(LLVMContextRef ctx, LLVMModuleRef module,
                     LLVMBuilderRef builder, LLVMTypeRef *return_types,
                     unsigned num_return_elems, LLVMTypeRef *param_types,
                     unsigned param_count, unsigned array_params,
                     unsigned sgpr_params)
{
	LLVMTypeRef main_function_type, ret_type;
	LLVMBasicBlockRef main_function_body;

	if (num_return_elems)
		ret_type = LLVMStructTypeInContext(ctx, return_types,
		                                   num_return_elems, true);
	else
		ret_type = LLVMVoidTypeInContext(ctx);

	/* Setup the function */
	main_function_type =
	    LLVMFunctionType(ret_type, param_types, param_count, 0);
	LLVMValueRef main_function =
	    LLVMAddFunction(module, "main", main_function_type);
	main_function_body =
	    LLVMAppendBasicBlockInContext(ctx, main_function, "main_body");
	LLVMPositionBuilderAtEnd(builder, main_function_body);

	LLVMSetFunctionCallConv(main_function, RADEON_LLVM_AMDGPU_CS);
	for (unsigned i = 0; i < sgpr_params; ++i) {
		LLVMValueRef P = LLVMGetParam(main_function, i);

		if (i < array_params)
			LLVMAddAttribute(P, LLVMByValAttribute);
		else
			LLVMAddAttribute(P, LLVMInRegAttribute);
	}
	return main_function;
}

static LLVMTypeRef const_array(LLVMTypeRef elem_type, int num_elements)
{
	return LLVMPointerType(LLVMArrayType(elem_type, num_elements),
	                       CONST_ADDR_SPACE);
}

static LLVMValueRef to_integer(struct nir_to_llvm_context *ctx, LLVMValueRef v)
{
	LLVMTypeRef type = LLVMTypeOf(v);
	if (type == ctx->f32) {
		return LLVMBuildBitCast(ctx->builder, v, ctx->i32, "");
	} else if (LLVMGetTypeKind(type) == LLVMVectorTypeKind) {
		LLVMTypeRef elem_type = LLVMGetElementType(type);
		if (elem_type == ctx->f32) {
			LLVMTypeRef nt = LLVMVectorType(ctx->i32, LLVMGetVectorSize(type));
			return LLVMBuildBitCast(ctx->builder, v, nt, "");
		}
	}
	return v;
}

static LLVMValueRef to_float(struct nir_to_llvm_context *ctx, LLVMValueRef v)
{
	LLVMTypeRef type = LLVMTypeOf(v);
	if (type == ctx->i32) {
		return LLVMBuildBitCast(ctx->builder, v, ctx->f32, "");
	} else if (LLVMGetTypeKind(type) == LLVMVectorTypeKind) {
		LLVMTypeRef elem_type = LLVMGetElementType(type);
		if (elem_type == ctx->i32) {
			LLVMTypeRef nt = LLVMVectorType(ctx->f32, LLVMGetVectorSize(type));
			return LLVMBuildBitCast(ctx->builder, v, nt, "");
		}
	}
	return v;
}

static LLVMValueRef build_indexed_load(struct nir_to_llvm_context *ctx,
				       LLVMValueRef base_ptr, LLVMValueRef index,
				       bool uniform)
{
	LLVMValueRef pointer;
	LLVMValueRef indices[] = {ctx->i32zero, index};

	pointer = LLVMBuildGEP(ctx->builder, base_ptr, indices, 2, "");
	if (uniform)
		LLVMSetMetadata(pointer, ctx->uniform_md_kind, ctx->empty_md);
	return LLVMBuildLoad(ctx->builder, pointer, "");
}

static LLVMValueRef build_indexed_load_const(struct nir_to_llvm_context *ctx,
					     LLVMValueRef base_ptr, LLVMValueRef index)
{
	LLVMValueRef result = build_indexed_load(ctx, base_ptr, index, true);
	LLVMSetMetadata(result, 1, ctx->const_md);
	return result;
}

static void create_function(struct nir_to_llvm_context *ctx,
                            struct nir_shader *nir)
{
	LLVMTypeRef arg_types[23];
	unsigned arg_idx = 0;
	unsigned array_count = 0;
	unsigned sgpr_count = 0, user_sgpr_count;
	unsigned i;
	for (unsigned i = 0; i < 4; ++i)
		arg_types[arg_idx++] = const_array(ctx->i8, 1024 * 1024);

	arg_types[arg_idx++] = const_array(ctx->i8, 1024 * 1024);

	array_count = arg_idx;
	switch (nir->stage) {
	case MESA_SHADER_COMPUTE:
		arg_types[arg_idx++] = LLVMVectorType(ctx->i32, 3); /* grid size */
		user_sgpr_count = arg_idx;
		arg_types[arg_idx++] = LLVMVectorType(ctx->i32, 3);
		sgpr_count = arg_idx;

		arg_types[arg_idx++] = LLVMVectorType(ctx->i32, 3);
		break;
	case MESA_SHADER_VERTEX:
		arg_types[arg_idx++] = const_array(ctx->v16i8, 16);
		arg_types[arg_idx++] = ctx->i32; // base vertex
		arg_types[arg_idx++] = ctx->i32; // start instance
		user_sgpr_count = sgpr_count = arg_idx;
		arg_types[arg_idx++] = ctx->i32; // vertex id
		arg_types[arg_idx++] = ctx->i32; // rel auto id
		arg_types[arg_idx++] = ctx->i32; // vs prim id
		arg_types[arg_idx++] = ctx->i32; // instance id
		break;
	case MESA_SHADER_FRAGMENT:
		user_sgpr_count = arg_idx;
		arg_types[arg_idx++] = ctx->i32; /* prim mask */
		sgpr_count = arg_idx;
		arg_types[arg_idx++] = ctx->v2i32; /* persp sample */
		arg_types[arg_idx++] = ctx->v2i32; /* persp center */
		arg_types[arg_idx++] = ctx->v2i32; /* persp centroid */
		arg_types[arg_idx++] = ctx->v3i32; /* persp pull model */
		arg_types[arg_idx++] = ctx->v2i32; /* linear sample */
		arg_types[arg_idx++] = ctx->v2i32; /* linear center */
		arg_types[arg_idx++] = ctx->v2i32; /* linear centroid */
		arg_types[arg_idx++] = ctx->f32;  /* line stipple tex */
		arg_types[arg_idx++] = ctx->f32;  /* pos x float */
		arg_types[arg_idx++] = ctx->f32;  /* pos y float */
		arg_types[arg_idx++] = ctx->f32;  /* pos z float */
		arg_types[arg_idx++] = ctx->f32;  /* pos w float */
		arg_types[arg_idx++] = ctx->i32;  /* front face */
		arg_types[arg_idx++] = ctx->i32;  /* ancillary */
		arg_types[arg_idx++] = ctx->f32;  /* sample coverage */
		arg_types[arg_idx++] = ctx->i32;  /* fixed pt */
		break;
	default:
		unreachable("Shader stage not implemented");
	}

	ctx->main_function = create_llvm_function(
	    ctx->context, ctx->module, ctx->builder, NULL, 0, arg_types,
	    arg_idx, array_count, sgpr_count);
	set_llvm_calling_convention(ctx->main_function, nir->stage);


	ctx->shader_info->num_input_sgprs = 0;
	ctx->shader_info->num_input_vgprs = 0;

	for (i = 0; i < user_sgpr_count; i++)
		ctx->shader_info->num_user_sgprs += llvm_get_type_size(arg_types[i]) / 4;

	ctx->shader_info->num_input_sgprs = ctx->shader_info->num_user_sgprs;
	for (; i < sgpr_count; i++)
		ctx->shader_info->num_input_sgprs += llvm_get_type_size(arg_types[i]) / 4;

	if (nir->stage != MESA_SHADER_FRAGMENT)
		for (; i < arg_idx; ++i)
			ctx->shader_info->num_input_vgprs += llvm_get_type_size(arg_types[i]) / 4;

	arg_idx = 0;
	for (unsigned i = 0; i < 4; ++i)
		ctx->descriptor_sets[i] =
		    LLVMGetParam(ctx->main_function, arg_idx++);

	ctx->push_constants = LLVMGetParam(ctx->main_function, arg_idx++);

	switch (nir->stage) {
	case MESA_SHADER_COMPUTE:
		ctx->num_work_groups =
		    LLVMGetParam(ctx->main_function, arg_idx++);
		ctx->workgroup_ids =
		    LLVMGetParam(ctx->main_function, arg_idx++);
		ctx->local_invocation_ids =
		    LLVMGetParam(ctx->main_function, arg_idx++);
		break;
	case MESA_SHADER_VERTEX:
		ctx->vertex_buffers = LLVMGetParam(ctx->main_function, arg_idx++);
		ctx->base_vertex = LLVMGetParam(ctx->main_function, arg_idx++);
		ctx->start_instance = LLVMGetParam(ctx->main_function, arg_idx++);
		ctx->vertex_id = LLVMGetParam(ctx->main_function, arg_idx++);
		ctx->rel_auto_id = LLVMGetParam(ctx->main_function, arg_idx++);
		ctx->vs_prim_id = LLVMGetParam(ctx->main_function, arg_idx++);
		ctx->instance_id = LLVMGetParam(ctx->main_function, arg_idx++);
		break;
	case MESA_SHADER_FRAGMENT:
		ctx->prim_mask = LLVMGetParam(ctx->main_function, arg_idx++);

		ctx->persp_sample = LLVMGetParam(ctx->main_function, arg_idx++);
		ctx->persp_center = LLVMGetParam(ctx->main_function, arg_idx++);
		ctx->persp_centroid = LLVMGetParam(ctx->main_function, arg_idx++);
		arg_idx++;
		ctx->linear_sample = LLVMGetParam(ctx->main_function, arg_idx++);
		ctx->linear_center = LLVMGetParam(ctx->main_function, arg_idx++);
		ctx->linear_centroid = LLVMGetParam(ctx->main_function, arg_idx++);
		break;
	default:
		unreachable("Shader stage not implemented");
	}
}

static void setup_types(struct nir_to_llvm_context *ctx)
{
	LLVMValueRef args[4];

	ctx->voidt = LLVMVoidTypeInContext(ctx->context);
	ctx->i1 = LLVMIntTypeInContext(ctx->context, 1);
	ctx->i8 = LLVMIntTypeInContext(ctx->context, 8);
	ctx->i16 = LLVMIntTypeInContext(ctx->context, 16);
	ctx->i32 = LLVMIntTypeInContext(ctx->context, 32);
	ctx->v2i32 = LLVMVectorType(ctx->i32, 2);
	ctx->v3i32 = LLVMVectorType(ctx->i32, 3);
	ctx->v4i32 = LLVMVectorType(ctx->i32, 4);
	ctx->v8i32 = LLVMVectorType(ctx->i32, 8);
	ctx->f32 = LLVMFloatTypeInContext(ctx->context);
	ctx->v4f32 = LLVMVectorType(ctx->f32, 4);
	ctx->v16i8 = LLVMVectorType(ctx->i8, 16);

	ctx->i32zero = LLVMConstInt(ctx->i32, 0, false);
	ctx->i32one = LLVMConstInt(ctx->i32, 1, false);
	ctx->f32zero = LLVMConstReal(ctx->f32, 0.0);
	ctx->f32one = LLVMConstReal(ctx->f32, 1.0);

	args[0] = ctx->f32zero;
	args[1] = ctx->f32zero;
	args[2] = ctx->f32zero;
	args[3] = ctx->f32one;
	ctx->v4f32empty = LLVMConstVector(args, 4);
	args[0] = LLVMMDStringInContext(ctx->context, "const", 5);
	args[1] = 0;
	args[2] = LLVMConstInt(ctx->i32, 1, 0);
	ctx->const_md = LLVMMDNodeInContext(ctx->context, args, 3);

	ctx->uniform_md_kind =
	    LLVMGetMDKindIDInContext(ctx->context, "amdgpu.uniform", 14);
	ctx->empty_md = LLVMMDNodeInContext(ctx->context, NULL, 0);
}

static int get_llvm_num_components(LLVMValueRef value)
{
	LLVMTypeRef type = LLVMTypeOf(value);
	unsigned num_components = LLVMGetTypeKind(type) == LLVMVectorTypeKind
	                              ? LLVMGetVectorSize(type)
	                              : 1;
	return num_components;
}
static LLVMValueRef trim_vector(struct nir_to_llvm_context *ctx,
                                LLVMValueRef value, unsigned count)
{
	unsigned num_components = get_llvm_num_components(value);
	if (count == num_components)
		return value;

	LLVMValueRef masks[] = {
	    LLVMConstInt(ctx->i32, 0, false), LLVMConstInt(ctx->i32, 1, false),
	    LLVMConstInt(ctx->i32, 2, false), LLVMConstInt(ctx->i32, 3, false)};

	if (count == 1)
		return LLVMBuildExtractElement(ctx->builder, value, masks[0],
		                               "");

	LLVMValueRef swizzle = LLVMConstVector(masks, count);
	return LLVMBuildShuffleVector(ctx->builder, value, value, swizzle, "");
}

static LLVMValueRef
build_gather_values_extended(struct nir_to_llvm_context *ctx,
			     LLVMValueRef *values,
			     unsigned value_count,
			     unsigned value_stride,
			     bool load)
{
	LLVMBuilderRef builder = ctx->builder;
	LLVMValueRef vec;
	unsigned i;


	if (value_count == 1) {
		if (load)
			return LLVMBuildLoad(builder, values[0], "");
		return values[0];
	}

	for (i = 0; i < value_count; i++) {
		LLVMValueRef value = values[i * value_stride];
		if (load)
			value = LLVMBuildLoad(builder, value, "");

		if (!i)
			vec = LLVMGetUndef( LLVMVectorType(LLVMTypeOf(value), value_count));
		LLVMValueRef index = LLVMConstInt(ctx->i32, i, false);
		vec = LLVMBuildInsertElement(builder, vec, value, index, "");
	}
	return vec;
}


static void
build_store_values_extended(struct nir_to_llvm_context *ctx,
			     LLVMValueRef *values,
			     unsigned value_count,
			     unsigned value_stride,
			     LLVMValueRef vec)
{
	LLVMBuilderRef builder = ctx->builder;
	unsigned i;

	if (value_count == 1) {
		LLVMBuildLoad(builder, vec, values[0]);
		return;
	}

	for (i = 0; i < value_count; i++) {
		LLVMValueRef ptr = values[i * value_stride];
		LLVMValueRef index = LLVMConstInt(ctx->i32, i, false);
		LLVMValueRef value = LLVMBuildExtractElement(builder, vec, index, "");
		LLVMBuildStore(builder, value, ptr);
	}
}

static LLVMValueRef
build_gather_values(struct nir_to_llvm_context *ctx,
		    LLVMValueRef *values,
		    unsigned value_count)
{
	return build_gather_values_extended(ctx, values, value_count, 1, false);
}

static LLVMTypeRef get_def_type(struct nir_to_llvm_context *ctx,
                                nir_ssa_def *def)
{
	LLVMTypeRef type = LLVMIntTypeInContext(ctx->context, def->bit_size);
	if (def->num_components > 1) {
		type = LLVMVectorType(type, def->num_components);
	}
	return type;
}

static LLVMValueRef get_src(struct nir_to_llvm_context *ctx, nir_src src)
{
	assert(src.is_ssa);
	struct hash_entry *entry = _mesa_hash_table_search(ctx->defs, src.ssa);
	return (LLVMValueRef)entry->data;
}


static LLVMBasicBlockRef get_block(struct nir_to_llvm_context *ctx,
                                   struct nir_block *b)
{
	struct hash_entry *entry = _mesa_hash_table_search(ctx->defs, b);
	return (LLVMBasicBlockRef)entry->data;
}

static LLVMValueRef get_alu_src(struct nir_to_llvm_context *ctx,
                                nir_alu_src src,
                                unsigned num_components)
{
	LLVMValueRef value = get_src(ctx, src.src);
	bool need_swizzle = false;

	assert(value);
	LLVMTypeRef type = LLVMTypeOf(value);
	unsigned src_components = LLVMGetTypeKind(type) == LLVMVectorTypeKind
	                              ? LLVMGetVectorSize(type)
	                              : 1;

	for (unsigned i = 0; i < num_components; ++i) {
		assert(src.swizzle[i] < src_components);
		if (src.swizzle[i] != i)
			need_swizzle = true;
	}

	if (need_swizzle || num_components != src_components) {
		LLVMValueRef masks[] = {
		    LLVMConstInt(ctx->i32, src.swizzle[0], false),
		    LLVMConstInt(ctx->i32, src.swizzle[1], false),
		    LLVMConstInt(ctx->i32, src.swizzle[2], false),
		    LLVMConstInt(ctx->i32, src.swizzle[3], false)};

		if (src_components > 1 && num_components == 1) {
			value = LLVMBuildExtractElement(ctx->builder, value,
			                                masks[0], "");
		} else if (src_components == 1 && num_components > 1) {
			LLVMValueRef values[] = {value, value, value, value};
			value = build_gather_values(ctx, values, num_components);
		} else {
			LLVMValueRef swizzle = LLVMConstVector(masks, num_components);
			value = LLVMBuildShuffleVector(ctx->builder, value, value,
		                                       swizzle, "");
		}
	}
	assert(!src.negate);
	assert(!src.abs);
	return value;
}

static LLVMValueRef emit_int_cmp(struct nir_to_llvm_context *ctx,
                                 LLVMIntPredicate pred, LLVMValueRef src0,
                                 LLVMValueRef src1)
{
	LLVMValueRef result = LLVMBuildICmp(ctx->builder, pred, src0, src1, "");
	return LLVMBuildSelect(ctx->builder, result,
	                       LLVMConstInt(ctx->i32, 0xFFFFFFFF, false),
	                       LLVMConstInt(ctx->i32, 0, false), "");
}

static LLVMValueRef emit_float_cmp(struct nir_to_llvm_context *ctx,
                                   LLVMRealPredicate pred, LLVMValueRef src0,
                                   LLVMValueRef src1)
{
	LLVMValueRef result;
	src0 = to_float(ctx, src0);
	src1 = to_float(ctx, src1);
	result = LLVMBuildFCmp(ctx->builder, pred, src0, src1, "");
	return LLVMBuildSelect(ctx->builder, result,
	                       LLVMConstInt(ctx->i32, 0xFFFFFFFF, false),
	                       LLVMConstInt(ctx->i32, 0, false), "");
}

static LLVMValueRef emit_intrin_1f_param(struct nir_to_llvm_context *ctx,
					 const char *intrin,
					 LLVMValueRef src0)
{
	LLVMValueRef params[] = {
		to_float(ctx, src0),
	};
	return emit_llvm_intrinsic(ctx, intrin, ctx->f32, params, 1, LLVMReadNoneAttribute);
}

static LLVMValueRef emit_intrin_2f_param(struct nir_to_llvm_context *ctx,
				       const char *intrin,
				       LLVMValueRef src0, LLVMValueRef src1)
{
	LLVMValueRef params[] = {
		to_float(ctx, src0),
		to_float(ctx, src1),
	};
	return emit_llvm_intrinsic(ctx, intrin, ctx->f32, params, 2, LLVMReadNoneAttribute);
}

static LLVMValueRef emit_intrin_3f_param(struct nir_to_llvm_context *ctx,
					 const char *intrin,
					 LLVMValueRef src0, LLVMValueRef src1, LLVMValueRef src2)
{
	LLVMValueRef params[] = {
		to_float(ctx, src0),
		to_float(ctx, src1),
		to_float(ctx, src2),
	};
	return emit_llvm_intrinsic(ctx, intrin, ctx->f32, params, 3, LLVMReadNoneAttribute);
}

static LLVMValueRef emit_bcsel(struct nir_to_llvm_context *ctx,
			       LLVMValueRef src0, LLVMValueRef src1, LLVMValueRef src2)
{
	LLVMValueRef v = LLVMBuildICmp(ctx->builder, LLVMIntNE, src0,
				       ctx->i32zero, "");
	return LLVMBuildSelect(ctx->builder, v, src1, src2, "");
}

static LLVMValueRef emit_find_lsb(struct nir_to_llvm_context *ctx,
				  LLVMValueRef src0)
{
	LLVMValueRef params[2] = {
		src0,

		/* The value of 1 means that ffs(x=0) = undef, so LLVM won't
		 * add special code to check for x=0. The reason is that
		 * the LLVM behavior for x=0 is different from what we
		 * need here.
		 *
		 * The hardware already implements the correct behavior.
		 */
		LLVMConstInt(ctx->i32, 1, false),
	};
	return emit_llvm_intrinsic(ctx, "llvm.cttz.i32", ctx->i32, params, 2, LLVMReadNoneAttribute);
}

static LLVMValueRef emit_minmax_int(struct nir_to_llvm_context *ctx,
				    LLVMIntPredicate pred,
				    LLVMValueRef src0, LLVMValueRef src1)
{
	return LLVMBuildSelect(ctx->builder,
			       LLVMBuildICmp(ctx->builder, pred, src0, src1, ""),
			       src0,
			       src1, "");

}
static LLVMValueRef emit_iabs(struct nir_to_llvm_context *ctx,
			      LLVMValueRef src0)
{
	return emit_minmax_int(ctx, LLVMIntSGT, src0,
			       LLVMBuildNeg(ctx->builder, src0, ""));
}

static LLVMValueRef emit_fsign(struct nir_to_llvm_context *ctx,
			       LLVMValueRef src0)
{
	LLVMValueRef cmp, val;

	cmp = LLVMBuildFCmp(ctx->builder, LLVMRealOGT, src0, ctx->f32zero, "");
	val = LLVMBuildSelect(ctx->builder, cmp, ctx->f32one, src0, "");
	cmp = LLVMBuildFCmp(ctx->builder, LLVMRealOGE, val, ctx->f32zero, "");
	val = LLVMBuildSelect(ctx->builder, cmp, val, LLVMConstReal(ctx->f32, -1.0), "");
	return val;
}

static LLVMValueRef emit_isign(struct nir_to_llvm_context *ctx,
			       LLVMValueRef src0)
{
	LLVMValueRef cmp, val;

	cmp = LLVMBuildICmp(ctx->builder, LLVMIntSGT, src0, ctx->i32zero, "");
	val = LLVMBuildSelect(ctx->builder, cmp, ctx->i32one, src0, "");
	cmp = LLVMBuildICmp(ctx->builder, LLVMIntSGE, val, ctx->i32zero, "");
	val = LLVMBuildSelect(ctx->builder, cmp, val, LLVMConstInt(ctx->i32, -1, true), "");
	return val;
}

static LLVMValueRef emit_ffract(struct nir_to_llvm_context *ctx,
				LLVMValueRef src0)
{
	const char *intr = "llvm.floor.f32";
	LLVMValueRef fsrc0 = to_float(ctx, src0);
	LLVMValueRef params[] = {
		fsrc0,
	};
	LLVMValueRef floor = emit_llvm_intrinsic(ctx, intr,
						 ctx->f32, params, 1,
						 LLVMReadNoneAttribute);
	return LLVMBuildFSub(ctx->builder, fsrc0, floor, "");
}

static LLVMValueRef emit_uint_carry(struct nir_to_llvm_context *ctx,
				    const char *intrin,
				    LLVMValueRef src0, LLVMValueRef src1)
{
	LLVMTypeRef ret_type;
	LLVMTypeRef types[] = { ctx->i32, ctx->i1 };
	LLVMValueRef res;
	LLVMValueRef params[] = { src0, src1 };
	ret_type = LLVMStructTypeInContext(ctx->context, types,
					   2, true);

	res = emit_llvm_intrinsic(ctx, intrin, ret_type,
				  params, 2, LLVMReadNoneAttribute);

	res = LLVMBuildExtractValue(ctx->builder, res, 1, "");
	res = LLVMBuildZExt(ctx->builder, res, ctx->i32, "");
	return res;
}

static LLVMValueRef emit_b2f(struct nir_to_llvm_context *ctx,
			     LLVMValueRef src0)
{
	return LLVMBuildAnd(ctx->builder, src0, LLVMBuildBitCast(ctx->builder, LLVMConstReal(ctx->f32, 1.0), ctx->i32, ""), "");
}

static void visit_alu(struct nir_to_llvm_context *ctx, nir_alu_instr *instr)
{
	LLVMValueRef src[4], result = NULL;
	unsigned num_components = instr->dest.dest.ssa.num_components;
	unsigned src_components;

	assert(nir_op_infos[instr->op].num_inputs <= ARRAY_SIZE(src));
	switch (instr->op) {
	case nir_op_vec2:
	case nir_op_vec3:
	case nir_op_vec4:
		src_components = 1;
		break;
	default:
		src_components = num_components;
		break;
	}
	for (unsigned i = 0; i < nir_op_infos[instr->op].num_inputs; i++)
		src[i] = get_alu_src(ctx, instr->src[i], src_components);

	switch (instr->op) {
	case nir_op_fmov:
	case nir_op_imov:
		result = src[0];
		break;
	case nir_op_fneg:
	        src[0] = to_float(ctx, src[0]);
		result = LLVMBuildFNeg(ctx->builder, src[0], "");
		break;
	case nir_op_ineg:
		result = LLVMBuildNeg(ctx->builder, src[0], "");
		break;
	case nir_op_inot:
		result = LLVMBuildNot(ctx->builder, src[0], "");
		break;
	case nir_op_iadd:
		result = LLVMBuildAdd(ctx->builder, src[0], src[1], "");
		break;
	case nir_op_fadd:
		src[0] = to_float(ctx, src[0]);
		src[1] = to_float(ctx, src[1]);
		result = LLVMBuildFAdd(ctx->builder, src[0], src[1], "");
		break;
	case nir_op_fsub:
		src[0] = to_float(ctx, src[0]);
		src[1] = to_float(ctx, src[1]);
		result = LLVMBuildFSub(ctx->builder, src[0], src[1], "");
		break;
	case nir_op_isub:
		result = LLVMBuildSub(ctx->builder, src[0], src[1], "");
		break;
	case nir_op_imul:
		result = LLVMBuildMul(ctx->builder, src[0], src[1], "");
		break;
	case nir_op_imod:
		result = LLVMBuildSRem(ctx->builder, src[0], src[1], "");
		break;
	case nir_op_umod:
		result = LLVMBuildURem(ctx->builder, src[0], src[1], "");
		break;
	case nir_op_fmod:
		src[0] = to_float(ctx, src[0]);
		src[1] = to_float(ctx, src[1]);
		result = LLVMBuildFRem(ctx->builder, src[0], src[1], "");
		break;
	case nir_op_idiv:
		result = LLVMBuildSDiv(ctx->builder, src[0], src[1], "");
		break;
	case nir_op_udiv:
		result = LLVMBuildUDiv(ctx->builder, src[0], src[1], "");
		break;
	case nir_op_fmul:
		src[0] = to_float(ctx, src[0]);
		src[1] = to_float(ctx, src[1]);
		result = LLVMBuildFMul(ctx->builder, src[0], src[1], "");
		break;
	case nir_op_fdiv:
		src[0] = to_float(ctx, src[0]);
		src[1] = to_float(ctx, src[1]);
		result = LLVMBuildFDiv(ctx->builder, src[0], src[1], "");
		break;
	case nir_op_frcp:
		src[0] = to_float(ctx, src[0]);
		result = LLVMBuildFDiv(ctx->builder, ctx->f32one, src[0], "");
		break;
	case nir_op_iand:
		result = LLVMBuildAnd(ctx->builder, src[0], src[1], "");
		break;
	case nir_op_ior:
		result = LLVMBuildOr(ctx->builder, src[0], src[1], "");
		break;
	case nir_op_ixor:
		result = LLVMBuildXor(ctx->builder, src[0], src[1], "");
		break;
	case nir_op_ishl:
		result = LLVMBuildShl(ctx->builder, src[0], src[1], "");
		break;
	case nir_op_ishr:
		result = LLVMBuildAShr(ctx->builder, src[0], src[1], "");
		break;
	case nir_op_ushr:
		result = LLVMBuildLShr(ctx->builder, src[0], src[1], "");
		break;
	case nir_op_ilt:
		result = emit_int_cmp(ctx, LLVMIntSLT, src[0], src[1]);
		break;
	case nir_op_ine:
		result = emit_int_cmp(ctx, LLVMIntNE, src[0], src[1]);
		break;
	case nir_op_ieq:
		result = emit_int_cmp(ctx, LLVMIntEQ, src[0], src[1]);
		break;
	case nir_op_ige:
		result = emit_int_cmp(ctx, LLVMIntSGE, src[0], src[1]);
		break;
	case nir_op_ult:
		result = emit_int_cmp(ctx, LLVMIntULT, src[0], src[1]);
		break;
	case nir_op_uge:
		result = emit_int_cmp(ctx, LLVMIntUGE, src[0], src[1]);
		break;
	case nir_op_feq:
		result = emit_float_cmp(ctx, LLVMRealOEQ, src[0], src[1]);
		break;
	case nir_op_fne:
		result = emit_float_cmp(ctx, LLVMRealONE, src[0], src[1]);
		break;
	case nir_op_flt:
		result = emit_float_cmp(ctx, LLVMRealOLT, src[0], src[1]);
		break;
	case nir_op_fge:
		result = emit_float_cmp(ctx, LLVMRealOGE, src[0], src[1]);
		break;
	case nir_op_fabs:
		result = emit_intrin_1f_param(ctx, "llvm.fabs.f32", src[0]);
		break;
	case nir_op_iabs:
		result = emit_iabs(ctx, src[0]);
		break;
	case nir_op_imax:
		result = emit_minmax_int(ctx, LLVMIntSGT, src[0], src[1]);
		break;
	case nir_op_imin:
		result = emit_minmax_int(ctx, LLVMIntSLT, src[0], src[1]);
		break;
	case nir_op_umax:
		result = emit_minmax_int(ctx, LLVMIntUGT, src[0], src[1]);
		break;
	case nir_op_umin:
		result = emit_minmax_int(ctx, LLVMIntULT, src[0], src[1]);
		break;
	case nir_op_isign:
		result = emit_isign(ctx, src[0]);
		break;
	case nir_op_fsign:
		src[0] = to_float(ctx, src[0]);
		result = emit_fsign(ctx, src[0]);
		break;
	case nir_op_ffloor:
		result = emit_intrin_1f_param(ctx, "llvm.floor.f32", src[0]);
		break;
	case nir_op_ftrunc:
		result = emit_intrin_1f_param(ctx, "llvm.trunc.f32", src[0]);
		break;
	case nir_op_fceil:
		result = emit_intrin_1f_param(ctx, "llvm.ceil.f32", src[0]);
		break;
	case nir_op_fround_even:
		result = emit_intrin_1f_param(ctx, "llvm.rint.f32", src[0]);
		break;
	case nir_op_ffract:
		result = emit_ffract(ctx, src[0]);
		break;
	case nir_op_fsin:
		result = emit_intrin_1f_param(ctx, "llvm.sin.f32", src[0]);
		break;
	case nir_op_fcos:
		result = emit_intrin_1f_param(ctx, "llvm.cos.f32", src[0]);
		break;
	case nir_op_fsqrt:
		result = emit_intrin_1f_param(ctx, "llvm.sqrt.f32", src[0]);
		break;
	case nir_op_fexp2:
		result = emit_intrin_1f_param(ctx, "llvm.exp2.f32", src[0]);
		break;
	case nir_op_frsq:
		result = emit_intrin_1f_param(ctx, "llvm.sqrt.f32", src[0]);
		result = LLVMBuildFDiv(ctx->builder, ctx->f32one, result, "");
		break;
	case nir_op_fpow:
		result = emit_intrin_2f_param(ctx, "llvm.pow.f32", src[0], src[1]);
		break;
	case nir_op_fmax:
		result = emit_intrin_2f_param(ctx, "llvm.maxnum.f32", src[0], src[1]);
		break;
	case nir_op_fmin:
		result = emit_intrin_2f_param(ctx, "llvm.minnum.f32", src[0], src[1]);
		break;
	case nir_op_ffma:
		result = emit_intrin_3f_param(ctx, "llvm.fma.f32", src[0], src[1], src[2]);
		break;
	case nir_op_vec2:
	case nir_op_vec3:
	case nir_op_vec4:
		for (unsigned i = 0; i < nir_op_infos[instr->op].num_inputs; i++)
			src[i] = to_integer(ctx, src[i]);
		result = build_gather_values(ctx, src, num_components);
		break;
	case nir_op_f2i:
		src[0] = to_float(ctx, src[0]);
		result = LLVMBuildFPToSI(ctx->builder, src[0], ctx->i32, "");
		break;
	case nir_op_f2u:
		src[0] = to_float(ctx, src[0]);
		result = LLVMBuildFPToUI(ctx->builder, src[0], ctx->i32, "");
		break;
	case nir_op_i2f:
		result = LLVMBuildSIToFP(ctx->builder, src[0], ctx->f32, "");
		break;
	case nir_op_u2f:
		result = LLVMBuildUIToFP(ctx->builder, src[0], ctx->f32, "");
		break;
	case nir_op_bcsel:
		result = emit_bcsel(ctx, src[0], src[1], src[2]);
		break;
	case nir_op_find_lsb:
		result = emit_find_lsb(ctx, src[0]);
		break;
	case nir_op_uadd_carry:
		result = emit_uint_carry(ctx, "llvm.uadd.with.overflow.i32", src[0], src[1]);
		break;
	case nir_op_usub_borrow:
		result = emit_uint_carry(ctx, "llvm.usub.with.overflow.i32", src[0], src[1]);
		break;
	case nir_op_b2f:
		result = emit_b2f(ctx, src[0]);
		break;
	default:
		fprintf(stderr, "Unknown NIR alu instr: ");
		nir_print_instr(&instr->instr, stderr);
		fprintf(stderr, "\n");
		abort();
	}

	if (result) {
		assert(instr->dest.dest.is_ssa);
		result = to_integer(ctx, result);
		_mesa_hash_table_insert(ctx->defs, &instr->dest.dest.ssa,
		                        result);
	}
}

static void visit_load_const(struct nir_to_llvm_context *ctx,
                             nir_load_const_instr *instr)
{
	LLVMValueRef values[4], value = NULL;
	LLVMTypeRef element_type =
	    LLVMIntTypeInContext(ctx->context, instr->def.bit_size);

	for (unsigned i = 0; i < instr->def.num_components; ++i) {
		switch (instr->def.bit_size) {
		case 32:
			values[i] = LLVMConstInt(element_type,
			                         instr->value.u32[i], false);
			break;
		case 64:
			values[i] = LLVMConstInt(element_type,
			                         instr->value.u64[i], false);
			break;
		default:
			fprintf(stderr,
			        "unsupported nir load_const bit_size: %d\n",
			        instr->def.bit_size);
			abort();
		}
	}
	if (instr->def.num_components > 1) {
		value = LLVMConstVector(values, instr->def.num_components);
	} else
		value = values[0];

	_mesa_hash_table_insert(ctx->defs, &instr->def, value);
}

static LLVMValueRef cast_ptr(struct nir_to_llvm_context *ctx, LLVMValueRef ptr,
                             LLVMTypeRef type)
{
	int addr_space = LLVMGetPointerAddressSpace(LLVMTypeOf(ptr));
	return LLVMBuildBitCast(ctx->builder, ptr,
	                        LLVMPointerType(type, addr_space), "");
}

static LLVMValueRef
emit_llvm_intrinsic(struct nir_to_llvm_context *ctx, const char *name,
                    LLVMTypeRef return_type, LLVMValueRef *params,
                    unsigned param_count, LLVMAttribute attribs)
{
	LLVMValueRef function;

	function = LLVMGetNamedFunction(ctx->module, name);
	if (!function) {
		LLVMTypeRef param_types[32], function_type;
		unsigned i;

		assert(param_count <= 32);

		for (i = 0; i < param_count; ++i) {
			assert(params[i]);
			param_types[i] = LLVMTypeOf(params[i]);
		}
		function_type =
		    LLVMFunctionType(return_type, param_types, param_count, 0);
		function = LLVMAddFunction(ctx->module, name, function_type);

		LLVMSetFunctionCallConv(function, LLVMCCallConv);
		LLVMSetLinkage(function, LLVMExternalLinkage);

		LLVMAddFunctionAttr(function, attribs | LLVMNoUnwindAttribute);
	}
	return LLVMBuildCall(ctx->builder, function, params, param_count, "");
}
/**
 * Given the i32 or vNi32 \p type, generate the textual name (e.g. for use with
 * intrinsic names).
 */
static void build_int_type_name(
	LLVMTypeRef type,
	char *buf, unsigned bufsize)
{
	assert(bufsize >= 6);

	if (LLVMGetTypeKind(type) == LLVMVectorTypeKind)
		snprintf(buf, bufsize, "v%ui32",
			 LLVMGetVectorSize(type));
	else
		strcpy(buf, "i32");
}

static LLVMValueRef build_tex_intrinsic(struct nir_to_llvm_context *ctx,
					nir_tex_instr *instr,
					struct ac_tex_info *tinfo)
{
	const char *name = "llvm.SI.image.sample";
	const char *infix = "";
	char intr_name[127];
	char type[64];

	switch (instr->op) {
	case nir_texop_txf:
		name = instr->sampler_dim == GLSL_SAMPLER_DIM_MS ?
			"llvm.SI.image.load" :
			"llvm.SI.image.load.mip";
		break;
	case nir_texop_txb:
		infix = ".b";
		break;
	default:
		break;
	}

	build_int_type_name(LLVMTypeOf(tinfo->args[0]), type, sizeof(type));
	sprintf(intr_name, "%s%s.%s", name, infix, type);

	return emit_llvm_intrinsic(ctx, intr_name, tinfo->dst_type, tinfo->args, tinfo->arg_count,
				   LLVMReadNoneAttribute | LLVMNoUnwindAttribute);

}

static LLVMValueRef visit_vulkan_resource_index(struct nir_to_llvm_context *ctx,
                                                nir_intrinsic_instr *instr)
{
	LLVMValueRef index = get_src(ctx, instr->src[0]);
	unsigned desc_set = nir_intrinsic_desc_set(instr);
	unsigned binding = nir_intrinsic_binding(instr);
	LLVMValueRef desc_ptr = ctx->descriptor_sets[desc_set];
	struct radv_descriptor_set_layout *layout = ctx->options->layout->set[desc_set].layout;
	unsigned base_offset = layout->binding[binding].offset;
	LLVMValueRef offset, stride;

	if (layout->binding[binding].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
	    layout->binding[binding].type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) {
		desc_ptr = ctx->push_constants;
		base_offset = ctx->options->layout->push_constant_size;
		base_offset +=  16 * layout->binding[binding].dynamic_offset_offset;
	}

	offset = LLVMConstInt(ctx->i32, base_offset, false);
	stride = LLVMConstInt(ctx->i32, layout->binding[binding].size, false);
	index = LLVMBuildMul(ctx->builder, index, stride, "");
	offset = LLVMBuildAdd(ctx->builder, offset, index, "");

	LLVMValueRef indices[] = {ctx->i32zero, offset};
	desc_ptr = LLVMBuildGEP(ctx->builder, desc_ptr, indices, 2, "");
	desc_ptr = cast_ptr(ctx, desc_ptr, ctx->v4i32);
	LLVMSetMetadata(desc_ptr, ctx->uniform_md_kind, ctx->empty_md);

	return LLVMBuildLoad(ctx->builder, desc_ptr, "");
}

static LLVMValueRef visit_load_push_constant(struct nir_to_llvm_context *ctx,
                                             nir_intrinsic_instr *instr)
{
	LLVMValueRef ptr;

	LLVMValueRef indices[] = {ctx->i32zero, get_src(ctx, instr->src[0])};
	ptr = LLVMBuildGEP(ctx->builder, ctx->push_constants, indices, 2, "");
	ptr = cast_ptr(ctx, ptr, get_def_type(ctx, &instr->dest.ssa));

	return LLVMBuildLoad(ctx->builder, ptr, "");
}

static void visit_store_ssbo(struct nir_to_llvm_context *ctx,
                             nir_intrinsic_instr *instr)
{
	const char *store_name;
	LLVMTypeRef data_type = ctx->f32;
	unsigned writemask = nir_intrinsic_write_mask(instr);
	LLVMValueRef base_data, base_offset;
	LLVMValueRef params[6];

	params[1] = get_src(ctx, instr->src[1]);
	params[2] = LLVMConstInt(ctx->i32, 0, false); /* vindex */
	params[4] = LLVMConstInt(ctx->i1, 0, false);  /* glc */
	params[5] = LLVMConstInt(ctx->i1, 0, false);  /* slc */

	base_data = get_src(ctx, instr->src[0]);

	if (instr->num_components > 1)
		data_type = LLVMVectorType(ctx->f32, instr->num_components);
	base_data = LLVMBuildBitCast(ctx->builder, get_src(ctx, instr->src[0]),
				     data_type, "");
	base_offset = get_src(ctx, instr->src[2]);      /* voffset */
	while (writemask) {
		int start, count;
		LLVMValueRef data;
		LLVMValueRef offset;
		LLVMValueRef tmp;
		u_bit_scan_consecutive_range(&writemask, &start, &count);

		/* Due to an LLVM limitation, split 3-element writes
		 * into a 2-element and a 1-element write. */
		if (count == 3) {
			writemask |= 1 << (start + 2);
			count = 2;
		}

		if (count == 4) {
			store_name = "llvm.amdgcn.buffer.store.v4f32";
			data = base_data;
		} else if (count == 2) {
			LLVMTypeRef v2f32 = LLVMVectorType(ctx->f32, 2);

			tmp = LLVMBuildExtractElement(ctx->builder,
						      base_data, LLVMConstInt(ctx->i32, start, false), "");
			data = LLVMBuildInsertElement(ctx->builder, LLVMGetUndef(v2f32), tmp,
						      ctx->i32zero, "");

			tmp = LLVMBuildExtractElement(ctx->builder,
						      base_data, LLVMConstInt(ctx->i32, start + 1, false), "");
			data = LLVMBuildInsertElement(ctx->builder, data, tmp,
						      ctx->i32one, "");
			store_name = "llvm.amdgcn.buffer.store.v2f32";

		} else {
			assert(count == 1);
			if (get_llvm_num_components(base_data) > 1)
				data = LLVMBuildExtractElement(ctx->builder, base_data,
							       LLVMConstInt(ctx->i32, start, false), "");
			else
				data = base_data;
			store_name = "llvm.amdgcn.buffer.store.f32";
		}

		offset = base_offset;
		if (start != 0) {
			offset = LLVMBuildAdd(ctx->builder, offset, LLVMConstInt(ctx->i32, start * 4, false), "");
		}
		params[0] = data;
		params[3] = offset;
		emit_llvm_intrinsic(ctx, store_name,
				    LLVMVoidTypeInContext(ctx->context), params, 6, 0);
	}
}

static LLVMValueRef visit_load_buffer(struct nir_to_llvm_context *ctx,
                                      nir_intrinsic_instr *instr)
{
	const nir_intrinsic_info *info = &nir_intrinsic_infos[instr->intrinsic];
	const char *load_name;
	LLVMTypeRef data_type = ctx->f32;
	if (instr->num_components == 3)
		data_type = LLVMVectorType(ctx->f32, 4);
	else if (instr->num_components > 1)
		data_type = LLVMVectorType(ctx->f32, instr->num_components);

	if (instr->num_components == 4 || instr->num_components == 3)
		load_name = "llvm.amdgcn.buffer.load.v4f32";
	else if (instr->num_components == 2)
		load_name = "llvm.amdgcn.buffer.load.v2f32";
	else if (instr->num_components == 1)
		load_name = "llvm.amdgcn.buffer.load.f32";
	else
		abort();

	LLVMValueRef params[] = {
	    get_src(ctx, instr->src[0]),
	    LLVMConstInt(ctx->i32, 0, false),
	    get_src(ctx, instr->src[1]),
	    LLVMConstInt(ctx->i1, 0, false),
	    LLVMConstInt(ctx->i1, 0, false),
	};

	LLVMValueRef ret =
	    emit_llvm_intrinsic(ctx, load_name, data_type, params, 5, 0);

	if (instr->num_components == 3)
		ret = trim_vector(ctx, ret, 3);

	return LLVMBuildBitCast(ctx->builder, ret,
	                        get_def_type(ctx, &instr->dest.ssa), "");
}

static void
radv_get_deref_offset(struct nir_to_llvm_context *ctx, nir_deref *tail,
                      bool vs_in, unsigned *const_out, LLVMValueRef *indir_out)
{
	unsigned const_offset = 0;
	LLVMValueRef offset = NULL;


	while (tail->child != NULL) {
		const struct glsl_type *parent_type = tail->type;
		tail = tail->child;

		if (tail->deref_type == nir_deref_type_array) {
			nir_deref_array *deref_array = nir_deref_as_array(tail);
			LLVMValueRef index, stride, local_offset;
			unsigned size = glsl_count_attribute_slots(tail->type, vs_in);

			const_offset += size * deref_array->base_offset;
			if (deref_array->deref_array_type == nir_deref_array_type_direct)
				continue;

			assert(deref_array->deref_array_type == nir_deref_array_type_indirect);
			index = get_src(ctx, deref_array->indirect);
			stride = LLVMConstInt(ctx->i32, size, 0);
			local_offset = LLVMBuildMul(ctx->builder, stride, index, "");

			if (offset)
				offset = LLVMBuildAdd(ctx->builder, offset, local_offset, "");
			else
				offset = local_offset;
		} else if (tail->deref_type == nir_deref_type_struct) {
			nir_deref_struct *deref_struct = nir_deref_as_struct(tail);

			for (unsigned i = 0; i < deref_struct->index; i++) {
				const struct glsl_type *ft = glsl_get_struct_field(parent_type, i);
				const_offset += glsl_count_attribute_slots(ft, vs_in);
			}
		} else
			unreachable("unsupported deref type");

	}

	if (const_offset && offset)
		offset = LLVMBuildAdd(ctx->builder, offset,
				      LLVMConstInt(ctx->i32, const_offset, 0),
				      "");

	*const_out = const_offset;
	*indir_out = offset;
}

static LLVMValueRef visit_load_var(struct nir_to_llvm_context *ctx,
				   nir_intrinsic_instr *instr)
{
	LLVMValueRef values[4];
	int idx = instr->variables[0]->var->data.driver_location;
	int ve = instr->dest.ssa.num_components;
	LLVMValueRef indir_index;
	unsigned const_index;
	switch (instr->variables[0]->var->data.mode) {
	case nir_var_shader_in:
		radv_get_deref_offset(ctx, &instr->variables[0]->deref,
				      ctx->stage == MESA_SHADER_VERTEX,
				      &const_index, &indir_index);
		for (unsigned chan = 0; chan < ve; chan++) {
			if (indir_index) {
				unsigned count = glsl_count_attribute_slots(
						instr->variables[0]->var->type,
						ctx->stage == MESA_SHADER_VERTEX);
				LLVMValueRef tmp_vec = build_gather_values_extended(
						ctx, ctx->inputs + idx + chan, count,
						4, false);

				values[chan] = LLVMBuildExtractElement(ctx->builder,
								       tmp_vec,
								       indir_index, "");
			} else
				values[chan] = ctx->inputs[idx + chan + const_index * 4];
		}
		return to_integer(ctx, build_gather_values(ctx, values, ve));
		break;
	case nir_var_local:
		for (unsigned chan = 0; chan < ve; chan++) {
			values[chan] = LLVMBuildLoad(ctx->builder, ctx->locals[idx + chan], "");
		}
		return to_integer(ctx, build_gather_values(ctx, values, ve));
	case nir_var_shader_out:
		radv_get_deref_offset(ctx, &instr->variables[0]->deref, false,
				      &const_index, &indir_index);
		for (unsigned chan = 0; chan < ve; chan++) {
			if (indir_index) {
				unsigned count = glsl_count_attribute_slots(
						instr->variables[0]->var->type, false);
				LLVMValueRef tmp_vec = build_gather_values_extended(
						ctx, ctx->outputs + idx + chan, count,
						4, true);

				values[chan] = LLVMBuildExtractElement(ctx->builder,
								       tmp_vec,
								       indir_index, "");
			} else {
			values[chan] = LLVMBuildLoad(ctx->builder,
						     ctx->outputs[idx + chan + const_index * 4],
						     "");
			}
		}
		return to_integer(ctx, build_gather_values(ctx, values, ve));
	default:
		break;
	}
	return NULL;
}

static void
visit_store_var(struct nir_to_llvm_context *ctx,
				   nir_intrinsic_instr *instr)
{
	LLVMValueRef temp_ptr, value;
	int idx = instr->variables[0]->var->data.driver_location;
	LLVMValueRef src = to_float(ctx, get_src(ctx, instr->src[0]));
	int writemask = instr->const_index[0];
	LLVMValueRef indir_index;
	unsigned const_index;
	switch (instr->variables[0]->var->data.mode) {
	case nir_var_shader_out:
		radv_get_deref_offset(ctx, &instr->variables[0]->deref, false,
				      &const_index, &indir_index);
		for (unsigned chan = 0; chan < 4; chan++) {
			if (!(writemask & (1 << chan)))
				continue;
			if (get_llvm_num_components(src) == 1)
				value = src;
			else
				value = LLVMBuildExtractElement(ctx->builder, src,
								LLVMConstInt(ctx->i32,
									     chan, false),
								"");

			if (indir_index) {
				unsigned count = glsl_count_attribute_slots(
						instr->variables[0]->var->type, false);
				LLVMValueRef tmp_vec = build_gather_values_extended(
						ctx, ctx->outputs + idx + chan, count,
						4, true);

				tmp_vec = LLVMBuildInsertElement(ctx->builder, tmp_vec,
								 value, indir_index, "");
				build_store_values_extended(ctx, ctx->outputs + idx + chan,
							    count, 4, tmp_vec);
			} else {
				temp_ptr = ctx->outputs[idx + chan + const_index * 4];

				LLVMBuildStore(ctx->builder, value, temp_ptr);
			}
		}
		break;
	case nir_var_local:
		for (unsigned chan = 0; chan < 4; chan++) {
			if (writemask & (1 << chan)) {
				temp_ptr = ctx->locals[idx + chan];

				if (get_llvm_num_components(src) == 1)
					value = src;
				else
					value = LLVMBuildExtractElement(ctx->builder, src, LLVMConstInt(ctx->i32, chan, false), "");
				LLVMBuildStore(ctx->builder, value, temp_ptr);
			}
		}
		break;
	default:
		break;
	}
}

static int image_type_to_components_count(enum glsl_sampler_dim dim, bool array)
{
	switch (dim) {
	case GLSL_SAMPLER_DIM_BUF:
		return 1;
	case GLSL_SAMPLER_DIM_1D:
		return array ? 2 : 1;
	case GLSL_SAMPLER_DIM_2D:
		return array ? 3 : 2;
	case GLSL_SAMPLER_DIM_3D:
		return 3;
	case GLSL_SAMPLER_DIM_RECT:
		return 2;
	}
	return 0;
}

static LLVMValueRef get_image_coords(struct nir_to_llvm_context *ctx,
				     nir_intrinsic_instr *instr)
{
	const nir_variable *var = instr->variables[0]->var;
	LLVMValueRef src0 = get_src(ctx, instr->src[0]);
	LLVMValueRef coords[4];
	LLVMValueRef masks[] = {
		LLVMConstInt(ctx->i32, 0, false), LLVMConstInt(ctx->i32, 1, false),
		LLVMConstInt(ctx->i32, 2, false), LLVMConstInt(ctx->i32, 3, false),
	};
	LLVMValueRef res;
	int count;
	count = image_type_to_components_count(glsl_get_sampler_dim(var->type),
					       glsl_sampler_type_is_array(var->type));

	if (count == 1)
		res = src0;
	else {
		int chan;
		for (chan = 0; chan < count; ++chan) {
			coords[chan] = LLVMBuildExtractElement(ctx->builder, src0, masks[chan], "");
		}
		if (count == 3) {
			coords[3] = LLVMGetUndef(ctx->i32);
			count = 4;
		}
		res = build_gather_values(ctx, coords, count);
	}
	return res;
}

static LLVMValueRef visit_image_load(struct nir_to_llvm_context *ctx,
				     nir_intrinsic_instr *instr)
{
	LLVMValueRef params[7];
	LLVMValueRef res;
	char intrinsic_name[32];
	char coords_type[8];
	const nir_variable *var = instr->variables[0]->var;

	params[0] = get_image_coords(ctx, instr);
	params[1] = get_sampler_desc(ctx, instr->variables[0], ctx->i32zero, DESC_IMAGE);
	params[2] = LLVMConstInt(ctx->i32, 15, false); /* dmask */
	params[3] = LLVMConstInt(ctx->i1, 0, false);  /* r128 */
	params[4] = glsl_sampler_type_is_array(var->type) ? ctx->i32one : ctx->i32zero; /* da */
	params[5] = LLVMConstInt(ctx->i1, 0, false);  /* glc */
	params[6] = LLVMConstInt(ctx->i1, 0, false);  /* slc */

	build_int_type_name(LLVMTypeOf(params[0]),
			    coords_type, sizeof(coords_type));

	snprintf(intrinsic_name, sizeof(intrinsic_name),
		 "llvm.amdgcn.image.load.%s", coords_type);
	res = emit_llvm_intrinsic(ctx, intrinsic_name, ctx->v4f32,
				  params, 7, LLVMReadOnlyAttribute);
	return res;
}

static void visit_image_store(struct nir_to_llvm_context *ctx,
			      nir_intrinsic_instr *instr)
{
	LLVMValueRef params[8];
	char intrinsic_name[32];
	char coords_type[8];
	const nir_variable *var = instr->variables[0]->var;

	if (glsl_get_sampler_dim(var->type) == GLSL_SAMPLER_DIM_BUF) {
		params[0] = to_float(ctx, get_src(ctx, instr->src[2])); /* data */
		params[1] = get_sampler_desc(ctx, instr->variables[0], ctx->i32zero, DESC_BUFFER);
		params[2] = LLVMBuildExtractElement(ctx->builder, get_src(ctx, instr->src[0]),
						    LLVMConstInt(ctx->i32, 0, false), ""); /* vindex */
		params[3] = LLVMConstInt(ctx->i32, 0, false); /* voffset */
		params[4] = LLVMConstInt(ctx->i1, 0, false);  /* glc */
		params[5] = LLVMConstInt(ctx->i1, 0, false);  /* slc */
		emit_llvm_intrinsic(ctx, "llvm.amdgcn.buffer.store.format.v4f32", ctx->voidt,
				    params, 6, 0);
	} else {
		params[0] = get_src(ctx, instr->src[2]); /* coords */
		params[1] = get_image_coords(ctx, instr);
		params[2] = get_sampler_desc(ctx, instr->variables[0], ctx->i32zero, DESC_IMAGE);
		params[3] = LLVMConstInt(ctx->i32, 15, false); /* dmask */
		params[4] = LLVMConstInt(ctx->i1, 0, false);  /* r128 */
		params[5] = glsl_sampler_type_is_array(var->type) ? ctx->i32one : ctx->i32zero; /* da */
		params[6] = LLVMConstInt(ctx->i1, 0, false);  /* glc */
		params[7] = LLVMConstInt(ctx->i1, 0, false);  /* slc */

		build_int_type_name(LLVMTypeOf(params[1]),
				    coords_type, sizeof(coords_type));

		snprintf(intrinsic_name, sizeof(intrinsic_name),
			 "llvm.amdgcn.image.store.%s", coords_type);
		emit_llvm_intrinsic(ctx, intrinsic_name, ctx->voidt,
				    params, 8, 0);
	}

}

static LLVMValueRef visit_image_size(struct nir_to_llvm_context *ctx,
				     nir_intrinsic_instr *instr)
{
	LLVMValueRef res;
	LLVMValueRef params[10];
	const nir_variable *var = instr->variables[0]->var;

	params[0] = ctx->i32zero;
	params[1] = get_sampler_desc(ctx, instr->variables[0], ctx->i32zero, DESC_IMAGE);
	params[2] = LLVMConstInt(ctx->i32, 15, false);
	params[3] = ctx->i32zero;
	params[4] = ctx->i32zero;
	params[5] = glsl_sampler_type_is_array(var->type) ? ctx->i32one : ctx->i32zero;
	params[6] = ctx->i32zero;
	params[7] = ctx->i32zero;
	params[8] = ctx->i32zero;
	params[9] = ctx->i32zero;

	res = emit_llvm_intrinsic(ctx, "llvm.SI.getresinfo.i32", ctx->v4i32,
				  params, 10, LLVMReadNoneAttribute);
	return res;
}

static void visit_intrinsic(struct nir_to_llvm_context *ctx,
                            nir_intrinsic_instr *instr)
{
	const nir_intrinsic_info *info = &nir_intrinsic_infos[instr->intrinsic];
	LLVMValueRef result = NULL;

	switch (instr->intrinsic) {
	case nir_intrinsic_load_work_group_id: {
		result = ctx->workgroup_ids;
		break;
	}
	case nir_intrinsic_load_base_vertex: {
		result = ctx->base_vertex;
		break;
	}
	case nir_intrinsic_load_vertex_id_zero_base: {
		result = ctx->vertex_id;
		break;
	}
	case nir_intrinsic_load_local_invocation_id: {
		result = ctx->local_invocation_ids;
		break;
	}
	case nir_intrinsic_load_base_instance:
		result = ctx->start_instance;
		break;
	case nir_intrinsic_load_instance_id:
		result = ctx->instance_id;
		break;
	case nir_intrinsic_load_num_work_groups:
		result = ctx->num_work_groups;
		break;
	case nir_intrinsic_load_push_constant:
		result = visit_load_push_constant(ctx, instr);
		break;
	case nir_intrinsic_vulkan_resource_index:
		result = visit_vulkan_resource_index(ctx, instr);
		break;
	case nir_intrinsic_store_ssbo:
		visit_store_ssbo(ctx, instr);
		break;
	case nir_intrinsic_load_ssbo:
		result = visit_load_buffer(ctx, instr);
		break;
	case nir_intrinsic_load_ubo:
		result = visit_load_buffer(ctx, instr);
		break;
	case nir_intrinsic_load_var:
		result = visit_load_var(ctx, instr);
		break;
	case nir_intrinsic_store_var:
		visit_store_var(ctx, instr);
		break;
	case nir_intrinsic_image_load:
		result = visit_image_load(ctx, instr);
		break;
	case nir_intrinsic_image_store:
		visit_image_store(ctx, instr);
		break;
	case nir_intrinsic_image_size:
		result = visit_image_size(ctx, instr);
		break;
	case nir_intrinsic_discard:
		ctx->shader_info->fs.can_discard = true;
		emit_llvm_intrinsic(ctx, "llvm.AMDGPU.kilp",
				    LLVMVoidTypeInContext(ctx->context),
				    NULL, 0, 0);
		break;
	default:
		fprintf(stderr, "Unknown intrinsic: ");
		nir_print_instr(&instr->instr, stderr);
		fprintf(stderr, "\n");
		break;
	}
	if (result) {
		assert(info->has_dest && instr->dest.is_ssa);
		_mesa_hash_table_insert(ctx->defs, &instr->dest.ssa, result);
	}
}

static LLVMValueRef get_sampler_desc(struct nir_to_llvm_context *ctx,
					  nir_deref_var *deref,
					  LLVMValueRef index,
					  enum desc_type desc_type)
{
	unsigned desc_set = deref->var->data.descriptor_set;
	LLVMValueRef list = ctx->descriptor_sets[desc_set];
	struct radv_descriptor_set_layout *layout = ctx->options->layout->set[desc_set].layout;
	struct radv_descriptor_set_binding_layout *binding = layout->binding + deref->var->data.binding;
	unsigned offset = binding->offset;
	unsigned stride = binding->size;
	unsigned type_size;
	LLVMBuilderRef builder = ctx->builder;
	LLVMTypeRef type;
	LLVMValueRef indices[2];

	assert(deref->var->data.binding < layout->binding_count);

	switch (desc_type) {
	case DESC_IMAGE:
		type = ctx->v8i32;
		type_size = 32;
		break;
	case DESC_FMASK:
		type = ctx->v8i32;
		offset += 32;
		type_size = 32;
		break;
	case DESC_SAMPLER:
		type = ctx->v4i32;
		if (binding->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
			offset += 64;

		type_size = 16;
		break;
	case DESC_BUFFER:
		type = ctx->v4i32;
		type_size = 16;
		break;
	}

	assert(stride % type_size == 0);

	index = LLVMBuildMul(builder, index, LLVMConstInt(ctx->i32, stride / type_size, 0), "");
	indices[0] = ctx->i32zero;
	indices[1] = LLVMConstInt(ctx->i32, offset, 0);
	list = LLVMBuildGEP(builder, list, indices, 2, "");
	list = LLVMBuildPointerCast(builder, list, const_array(type, 0), "");

	return build_indexed_load_const(ctx, list, index);
}

static void set_tex_fetch_args(struct nir_to_llvm_context *ctx,
			       struct ac_tex_info *tinfo,
			       nir_tex_instr *instr,
			       LLVMValueRef res_ptr, LLVMValueRef samp_ptr,
			       LLVMValueRef *param, unsigned count,
			       unsigned dmask)
{
	int num_args;
	unsigned is_rect = 0;
	LLVMValueRef coord;
	LLVMValueRef coord_vals[4];

	/* Pad to power of two vector */
	while (count < util_next_power_of_two(count))
		param[count++] = LLVMGetUndef(ctx->i32);

	if (count > 1)
		tinfo->args[0] = build_gather_values(ctx, param, count);
	else
		tinfo->args[0] = param[0];

	tinfo->args[1] = res_ptr;
	num_args = 2;

	if (instr->op == nir_texop_txf || instr->op == nir_texop_query_levels)
		tinfo->dst_type = ctx->v4i32;
	else {
		tinfo->dst_type = ctx->v4f32;
		tinfo->args[num_args++] = samp_ptr;
	}

	tinfo->args[num_args++] = LLVMConstInt(ctx->i32, dmask, 0);
	tinfo->args[num_args++] = LLVMConstInt(ctx->i32, is_rect, 0); /* unorm */
	tinfo->args[num_args++] = LLVMConstInt(ctx->i32, 0, 0); /* r128 */
	tinfo->args[num_args++] = LLVMConstInt(ctx->i32, instr->is_array, 0);
	tinfo->args[num_args++] = LLVMConstInt(ctx->i32, 0, 0); /* glc */
	tinfo->args[num_args++] = LLVMConstInt(ctx->i32, 0, 0); /* slc */
	tinfo->args[num_args++] = LLVMConstInt(ctx->i32, 0, 0); /* tfe */
	tinfo->args[num_args++] = LLVMConstInt(ctx->i32, 0, 0); /* lwe */

	tinfo->arg_count = num_args;
}

static void tex_fetch_ptrs(struct nir_to_llvm_context *ctx,
			   nir_tex_instr *instr,
			   LLVMValueRef *res_ptr, LLVMValueRef *samp_ptr,
			   LLVMValueRef *fmask_ptr)
{
	*res_ptr = get_sampler_desc(ctx, instr->texture, ctx->i32zero, DESC_IMAGE);
	if (samp_ptr && instr->sampler)
		*samp_ptr = get_sampler_desc(ctx, instr->sampler, ctx->i32zero, DESC_SAMPLER);
	if (fmask_ptr && instr->sampler)
		*fmask_ptr = get_sampler_desc(ctx, instr->texture, ctx->i32zero, DESC_FMASK);
}

static void visit_tex(struct nir_to_llvm_context *ctx, nir_tex_instr *instr)
{
	LLVMValueRef result = NULL;
	LLVMTypeRef dst_type;
	struct ac_tex_info tinfo = { 0 };
	unsigned dmask = 0xf;
	LLVMValueRef address[16];
	LLVMValueRef coords[5];
	LLVMValueRef coord;
	LLVMTypeRef data_type = ctx->i32;
	LLVMValueRef res_ptr, samp_ptr, fmask_ptr = NULL;
	unsigned chan, count = 0;
	LLVMValueRef masks[] = {
		LLVMConstInt(ctx->i32, 0, false), LLVMConstInt(ctx->i32, 1, false),
		LLVMConstInt(ctx->i32, 2, false), LLVMConstInt(ctx->i32, 3, false),
	};
	tex_fetch_ptrs(ctx, instr, &res_ptr, &samp_ptr, &fmask_ptr);

	coord = get_src(ctx, instr->src[0].src);

	if (instr->coord_components == 1)
		coords[0] = coord;
	else
		for (chan = 0; chan < instr->coord_components; chan++)
			coords[chan] = LLVMBuildExtractElement(ctx->builder, coord, masks[chan], "");

	/* TODO pack offsets */
	/* pack LOD bias value */
	if (instr->op == nir_texop_txb) {
		LLVMValueRef lod = get_src(ctx, instr->src[1].src);
		address[count++] = lod;
	}

	/* pack derivatives */
	address[count++] = coords[0];
	if (instr->coord_components > 1)
		address[count++] = coords[1];
	if (instr->coord_components > 2)
		address[count++] = coords[2];

	if ((instr->op == nir_texop_txl || instr->op == nir_texop_txf) && instr->num_srcs > 1) {
		LLVMValueRef lod = get_src(ctx, instr->src[1].src);
		address[count++] = lod;
	}

	for (chan = 0; chan < count; chan++) {
		address[chan] = LLVMBuildBitCast(ctx->builder,
						 address[chan], ctx->i32, "");
	}

	/* TODO sample FMASK magic */

	/* TODO TXF offset support */

	/* TODO TG4 support */
	set_tex_fetch_args(ctx, &tinfo, instr, res_ptr, samp_ptr, address, count, dmask);

	result = build_tex_intrinsic(ctx, instr, &tinfo);

	if (result) {
		assert(instr->dest.is_ssa);
		result = to_integer(ctx, result);
		_mesa_hash_table_insert(ctx->defs, &instr->dest.ssa, result);
	}
}


static void visit_phi(struct nir_to_llvm_context *ctx, nir_phi_instr *instr)
{
	LLVMTypeRef type = get_def_type(ctx, &instr->dest.ssa);
	LLVMValueRef result = LLVMBuildPhi(ctx->builder, type, "");

	_mesa_hash_table_insert(ctx->defs, &instr->dest.ssa, result);
	_mesa_hash_table_insert(ctx->phis, instr, result);
}

static void visit_post_phi(struct nir_to_llvm_context *ctx,
                           nir_phi_instr *instr,
                           LLVMValueRef llvm_phi)
{
	nir_phi_src *src;
	nir_foreach_phi_src(src, instr) {
		LLVMBasicBlockRef block = get_block(ctx, src->pred);
		LLVMValueRef llvm_src = get_src(ctx, src->src);

		LLVMAddIncoming(llvm_phi, &llvm_src, &block, 1);
	}
}

static void phi_post_pass(struct nir_to_llvm_context *ctx)
{
	struct hash_entry *entry;
	hash_table_foreach(ctx->phis, entry) {
		visit_post_phi(ctx, (nir_phi_instr*)entry->key,
		               (LLVMValueRef)entry->data);
	}
}


static void visit_ssa_undef(struct nir_to_llvm_context *ctx,
			    nir_ssa_undef_instr *instr)
{
	unsigned num_components = instr->def.num_components;
	LLVMValueRef undef;

	if (num_components == 1)
		undef = LLVMGetUndef(ctx->i32);
	else {
		undef = LLVMGetUndef(LLVMVectorType(ctx->i32, num_components));
	}
	_mesa_hash_table_insert(ctx->defs, &instr->def, undef);
}

static void visit_jump(struct nir_to_llvm_context *ctx,
		       nir_jump_instr *instr)
{
	switch (instr->type) {
	case nir_jump_break:
		LLVMBuildBr(ctx->builder, ctx->break_block);
		LLVMClearInsertionPosition(ctx->builder);
		break;
	case nir_jump_continue:
		LLVMBuildBr(ctx->builder, ctx->continue_block);
		LLVMClearInsertionPosition(ctx->builder);
		break;
	default:
		fprintf(stderr, "Unknown NIR jump instr: ");
		nir_print_instr(&instr->instr, stderr);
		fprintf(stderr, "\n");
		abort();
	}
}

static void visit_cf_list(struct nir_to_llvm_context *ctx,
                          struct exec_list *list);

static void visit_block(struct nir_to_llvm_context *ctx, nir_block *block)
{
	LLVMBasicBlockRef llvm_block = LLVMGetInsertBlock(ctx->builder);
	nir_foreach_instr(instr, block)
	{
		switch (instr->type) {
		case nir_instr_type_alu:
			visit_alu(ctx, nir_instr_as_alu(instr));
			break;
		case nir_instr_type_load_const:
			visit_load_const(ctx, nir_instr_as_load_const(instr));
			break;
		case nir_instr_type_intrinsic:
			visit_intrinsic(ctx, nir_instr_as_intrinsic(instr));
			break;
		case nir_instr_type_tex:
			visit_tex(ctx, nir_instr_as_tex(instr));
			break;
		case nir_instr_type_phi:
			visit_phi(ctx, nir_instr_as_phi(instr));
			break;
		case nir_instr_type_ssa_undef:
			visit_ssa_undef(ctx, nir_instr_as_ssa_undef(instr));
			break;
		case nir_instr_type_jump:
			visit_jump(ctx, nir_instr_as_jump(instr));
			break;
		default:
			fprintf(stderr, "Unknown NIR instr type: ");
			nir_print_instr(instr, stderr);
			fprintf(stderr, "\n");
			abort();
		}
	}

	_mesa_hash_table_insert(ctx->defs, block, llvm_block);
}

static void visit_if(struct nir_to_llvm_context *ctx, nir_if *if_stmt)
{
	LLVMValueRef value = get_src(ctx, if_stmt->condition);

	LLVMBasicBlockRef merge_block =
	    LLVMAppendBasicBlockInContext(ctx->context, ctx->main_function, "");
	LLVMBasicBlockRef if_block =
	    LLVMAppendBasicBlockInContext(ctx->context, ctx->main_function, "");
	LLVMBasicBlockRef else_block = merge_block;
	if (!exec_list_is_empty(&if_stmt->else_list))
		else_block = LLVMAppendBasicBlockInContext(
		    ctx->context, ctx->main_function, "");

	LLVMValueRef cond = LLVMBuildICmp(ctx->builder, LLVMIntNE, value,
	                                  LLVMConstInt(ctx->i32, 0, false), "");
	LLVMBuildCondBr(ctx->builder, cond, if_block, else_block);

	LLVMPositionBuilderAtEnd(ctx->builder, if_block);
	visit_cf_list(ctx, &if_stmt->then_list);
	if (LLVMGetInsertBlock(ctx->builder))
		LLVMBuildBr(ctx->builder, merge_block);

	if (!exec_list_is_empty(&if_stmt->else_list)) {
		LLVMPositionBuilderAtEnd(ctx->builder, else_block);
		visit_cf_list(ctx, &if_stmt->else_list);
		if (LLVMGetInsertBlock(ctx->builder))
			LLVMBuildBr(ctx->builder, merge_block);
	}

	LLVMPositionBuilderAtEnd(ctx->builder, merge_block);
}

static void visit_loop(struct nir_to_llvm_context *ctx, nir_loop *loop)
{
	LLVMBasicBlockRef continue_parent = ctx->continue_block;
	LLVMBasicBlockRef break_parent = ctx->break_block;

	ctx->continue_block =
	    LLVMAppendBasicBlockInContext(ctx->context, ctx->main_function, "");
	ctx->break_block =
	    LLVMAppendBasicBlockInContext(ctx->context, ctx->main_function, "");

	LLVMBuildBr(ctx->builder, ctx->continue_block);
	LLVMPositionBuilderAtEnd(ctx->builder, ctx->continue_block);
	visit_cf_list(ctx, &loop->body);

	if (LLVMGetInsertBlock(ctx->builder))
		LLVMBuildBr(ctx->builder, ctx->continue_block);
	LLVMPositionBuilderAtEnd(ctx->builder, ctx->break_block);

	ctx->continue_block = continue_parent;
	ctx->break_block = break_parent;
}

static void visit_cf_list(struct nir_to_llvm_context *ctx,
                          struct exec_list *list)
{
	foreach_list_typed(nir_cf_node, node, node, list)
	{
		switch (node->type) {
		case nir_cf_node_block:
			visit_block(ctx, nir_cf_node_as_block(node));
			break;

		case nir_cf_node_if:
			visit_if(ctx, nir_cf_node_as_if(node));
			break;

		case nir_cf_node_loop:
			visit_loop(ctx, nir_cf_node_as_loop(node));
			break;

		default:
			assert(0);
		}
	}
}

static void
handle_vs_input_decl(struct nir_to_llvm_context *ctx,
		     struct nir_variable *variable)
{
	LLVMValueRef t_list_ptr = ctx->vertex_buffers;
	LLVMValueRef t_offset;
	LLVMValueRef t_list;
	LLVMValueRef args[3];
	LLVMValueRef input;
	LLVMValueRef buffer_index;
	int index = variable->data.location - 17;
	int idx = variable->data.location;
	unsigned attrib_count = glsl_count_attribute_slots(variable->type, true);

	variable->data.driver_location = idx * 4;

	if (ctx->options->key.vs.instance_rate_inputs & (1u << index)) {
		buffer_index = LLVMBuildAdd(ctx->builder, ctx->instance_id,
					    ctx->start_instance, "");
		ctx->shader_info->vs.vgpr_comp_cnt = MAX2(3,
		                            ctx->shader_info->vs.vgpr_comp_cnt);
	} else
		buffer_index = LLVMBuildAdd(ctx->builder, ctx->vertex_id,
					    ctx->base_vertex, "");

	for (unsigned i = 0; i < attrib_count; ++i, ++idx) {
		t_offset = LLVMConstInt(ctx->i32, index, false);

		t_list = build_indexed_load_const(ctx, t_list_ptr, t_offset);
		args[0] = t_list;
		args[1] = LLVMConstInt(ctx->i32, 0, false);
		args[2] = buffer_index;
		input = emit_llvm_intrinsic(ctx,
			"llvm.SI.vs.load.input", ctx->v4f32, args, 3,
			LLVMReadNoneAttribute | LLVMNoUnwindAttribute);

		for (unsigned chan = 0; chan < 4; chan++) {
			LLVMValueRef llvm_chan = LLVMConstInt(ctx->i32, chan, false);
			ctx->inputs[radeon_llvm_reg_index_soa(idx, chan)] =
				to_integer(ctx, LLVMBuildExtractElement(ctx->builder,
							input, llvm_chan, ""));
		}
	}
}

static LLVMValueRef lookup_interp_param(struct nir_to_llvm_context *ctx,
					enum glsl_interp_mode interp, unsigned location)
{
	switch (interp) {
	case INTERP_MODE_FLAT:
	default:
		return NULL;
	case INTERP_MODE_SMOOTH:
	case INTERP_MODE_NONE:
		return ctx->persp_center;
	case INTERP_MODE_NOPERSPECTIVE:
		return ctx->linear_center;
	}
}

static void interp_fs_input(struct nir_to_llvm_context *ctx,
			    unsigned attr,
			    LLVMValueRef interp_param,
			    LLVMValueRef prim_mask,
			    LLVMValueRef result[4])
{
	const char *intr_name;
	LLVMValueRef attr_number;
	unsigned chan;

	attr_number = LLVMConstInt(ctx->i32, attr, false);

	/* fs.constant returns the param from the middle vertex, so it's not
	 * really useful for flat shading. It's meant to be used for custom
	 * interpolation (but the intrinsic can't fetch from the other two
	 * vertices).
	 *
	 * Luckily, it doesn't matter, because we rely on the FLAT_SHADE state
	 * to do the right thing. The only reason we use fs.constant is that
	 * fs.interp cannot be used on integers, because they can be equal
	 * to NaN.
	 */
	intr_name = interp_param ? "llvm.SI.fs.interp" : "llvm.SI.fs.constant";

	for (chan = 0; chan < 4; chan++) {
		LLVMValueRef args[4];
		LLVMValueRef llvm_chan = LLVMConstInt(ctx->i32, chan, false);

		args[0] = llvm_chan;
		args[1] = attr_number;
		args[2] = prim_mask;
		args[3] = interp_param;
		result[chan] = emit_llvm_intrinsic(ctx, intr_name,
						   ctx->f32, args, args[3] ? 4 : 3,
						  LLVMReadNoneAttribute | LLVMNoUnwindAttribute);
	}
}

static void
handle_fs_input_decl(struct nir_to_llvm_context *ctx,
		     struct nir_variable *variable)
{
	int idx = variable->data.location;
	unsigned attrib_count = glsl_count_attribute_slots(variable->type, false);
	LLVMValueRef interp;

	variable->data.driver_location = idx * 4;
	ctx->input_mask |= ((1ull << attrib_count) - 1) << variable->data.location;

	interp = lookup_interp_param(ctx, variable->data.interpolation, 0);

	for (unsigned i = 0; i < attrib_count; ++i)
		ctx->inputs[radeon_llvm_reg_index_soa(idx + i, 0)] = interp;

}

static void
handle_shader_input_decl(struct nir_to_llvm_context *ctx,
			 struct nir_variable *variable)
{
	switch (ctx->stage) {
	case MESA_SHADER_VERTEX:
		handle_vs_input_decl(ctx, variable);
		break;
	case MESA_SHADER_FRAGMENT:
		handle_fs_input_decl(ctx, variable);
		break;
	default:
		break;
	}

}

static void
handle_fs_inputs_pre(struct nir_to_llvm_context *ctx,
		     struct nir_shader *nir)
{
	unsigned index = 0;
	for (unsigned i = 0; i < RADEON_LLVM_MAX_INPUTS; ++i) {
		LLVMValueRef interp_param;
		unsigned attr = i - VARYING_SLOT_VAR0;
		if (!(ctx->input_mask & (1ull << i)))
			continue;

		interp_param = ctx->inputs[radeon_llvm_reg_index_soa(i, 0)];
		interp_fs_input(ctx, index, interp_param, ctx->prim_mask,
			&ctx->inputs[radeon_llvm_reg_index_soa(i, 0)]);

		if (!interp_param)
			ctx->shader_info->fs.flat_shaded_mask |= 1u << index;
		++index;
	}
	ctx->shader_info->fs.num_interp = index;
	ctx->shader_info->fs.input_mask = ctx->input_mask >> VARYING_SLOT_VAR0;
}

static LLVMValueRef
ac_build_alloca(struct nir_to_llvm_context *ctx,
                LLVMTypeRef type,
                const char *name)
{
	LLVMBuilderRef builder = ctx->builder;
	LLVMBasicBlockRef current_block = LLVMGetInsertBlock(builder);
	LLVMValueRef function = LLVMGetBasicBlockParent(current_block);
	LLVMBasicBlockRef first_block = LLVMGetEntryBasicBlock(function);
	LLVMValueRef first_instr = LLVMGetFirstInstruction(first_block);
	LLVMBuilderRef first_builder = LLVMCreateBuilderInContext(ctx->context);
	LLVMValueRef res;

	if (first_instr) {
		LLVMPositionBuilderBefore(first_builder, first_instr);
	} else {
		LLVMPositionBuilderAtEnd(first_builder, first_block);
	}

	res = LLVMBuildAlloca(first_builder, type, name);
	LLVMBuildStore(builder, LLVMConstNull(type), res);

	LLVMDisposeBuilder(first_builder);

	return res;
}

static LLVMValueRef si_build_alloca_undef(struct nir_to_llvm_context *ctx,
					  LLVMTypeRef type,
					  const char *name)
{
	LLVMValueRef ptr = ac_build_alloca(ctx, type, name);
	LLVMBuildStore(ctx->builder, LLVMGetUndef(type), ptr);
	return ptr;
}

static void
handle_shader_output_decl(struct nir_to_llvm_context *ctx,
			  struct nir_variable *variable)
{
	int idx = variable->data.location;
	unsigned attrib_count = glsl_count_attribute_slots(variable->type, false);

	variable->data.driver_location = idx * 4;
	for (unsigned i = 0; i < attrib_count; ++i) {
		for (unsigned chan = 0; chan < 4; chan++) {
			ctx->outputs[radeon_llvm_reg_index_soa(idx + i, chan)] =
		                       si_build_alloca_undef(ctx, ctx->f32, "");
		}
	}
	ctx->output_mask |= ((1ull << attrib_count) - 1) << variable->data.location;
}

static void
setup_locals(struct nir_to_llvm_context *ctx,
	     struct nir_function *func)
{
	int i, j;
	ctx->num_locals = 0;
	nir_foreach_variable(variable, &func->impl->locals) {
		variable->data.driver_location = ctx->num_locals * 4;
		ctx->num_locals++;
	}
	ctx->locals = malloc(4 * ctx->num_locals * sizeof(LLVMValueRef));
	if (!ctx->locals)
	    return;

	for (i = 0; i < ctx->num_locals; i++) {
		for (j = 0; j < 4; j++) {
			ctx->locals[i * 4 + j] =
				si_build_alloca_undef(ctx, ctx->f32, "temp");
		}
	}
}

/* Initialize arguments for the shader export intrinsic */
static void
si_llvm_init_export_args(struct nir_to_llvm_context *ctx,
			 LLVMValueRef *values,
			 unsigned target,
			 LLVMValueRef *args)
{
	LLVMValueRef val[4];

	/* Default is 0xf. Adjusted below depending on the format. */
	args[0] = LLVMConstInt(ctx->i32, 0xf, false);
	/* Specify whether the EXEC mask represents the valid mask */
	args[1] = LLVMConstInt(ctx->i32, 0, false);

	/* Specify whether this is the last export */
	args[2] = LLVMConstInt(ctx->i32, 0, false);
	/* Specify the target we are exporting */
	args[3] = LLVMConstInt(ctx->i32, target, false);

	args[4] = LLVMConstInt(ctx->i32, 0, false); /* COMPR flag */
	args[5] = LLVMGetUndef(ctx->f32);
	args[6] = LLVMGetUndef(ctx->f32);
	args[7] = LLVMGetUndef(ctx->f32);
	args[8] = LLVMGetUndef(ctx->f32);

	/* TODO expand this for frag shader */
	memcpy(&args[5], values, sizeof(values[0]) * 4);
}

static void
handle_vs_outputs_post(struct nir_to_llvm_context *ctx,
		      struct nir_shader *nir)
{
	uint32_t param_count = 0;
	struct si_shader_output_values *outputs;
	unsigned target;
	unsigned pos_idx, num_pos_exports = 0;
	int index;
	LLVMValueRef args[9];
	LLVMValueRef pos_args[4][9] = { { 0 } };
	int i;

	for (unsigned i = 0; i < RADEON_LLVM_MAX_OUTPUTS; ++i) {
		LLVMValueRef values[4];
		if (!(ctx->output_mask & (1ull << i)))
			continue;

		for (unsigned j = 0; j < 4; j++)
			values[j] = to_float(ctx, LLVMBuildLoad(ctx->builder,
					      ctx->outputs[radeon_llvm_reg_index_soa(i, j)], ""));

		if (i == VARYING_SLOT_POS)
			target = V_008DFC_SQ_EXP_POS;
		else if (i >= VARYING_SLOT_VAR0) {
			ctx->shader_info->vs.export_mask |= 1u << (i - VARYING_SLOT_VAR0);
			target = V_008DFC_SQ_EXP_PARAM + param_count;
			param_count++;
		}
		si_llvm_init_export_args(ctx, values, target, args);

		if (target >= V_008DFC_SQ_EXP_POS &&
		    target <= (V_008DFC_SQ_EXP_POS + 3)) {
			memcpy(pos_args[target - V_008DFC_SQ_EXP_POS],
			       args, sizeof(args));
		} else {
			emit_llvm_intrinsic(ctx,
					    "llvm.SI.export",
					    LLVMVoidTypeInContext(ctx->context),
					    args, 9, 0);
		}
	}

	/* We need to add the position output manually if it's missing. */
	if (!pos_args[0][0]) {
		pos_args[0][0] = LLVMConstInt(ctx->i32, 0xf, false);
		pos_args[0][1] = ctx->i32zero; /* EXEC mask */
		pos_args[0][2] = ctx->i32zero; /* last export? */
		pos_args[0][3] = LLVMConstInt(ctx->i32, V_008DFC_SQ_EXP_POS, false);
		pos_args[0][4] = ctx->i32zero; /* COMPR flag */
		pos_args[0][5] = ctx->f32zero; /* X */
		pos_args[0][6] = ctx->f32zero; /* Y */
		pos_args[0][7] = ctx->f32zero; /* Z */
		pos_args[0][8] = ctx->f32one;  /* W */
	}
	for (i = 0; i < 4; i++) {
		if (pos_args[i][0])
			num_pos_exports++;
	}

	pos_idx = 0;
	for (i = 0; i < 4; i++) {
		if (!pos_args[i][0])
			continue;

		/* Specify the target we are exporting */
		pos_args[i][3] = LLVMConstInt(ctx->i32, V_008DFC_SQ_EXP_POS + pos_idx++, false);
		if (pos_idx == num_pos_exports)
			pos_args[i][2] = ctx->i32one;
		emit_llvm_intrinsic(ctx,
				    "llvm.SI.export",
				    LLVMVoidTypeInContext(ctx->context),
				    pos_args[i], 9, 0);
	}

	ctx->shader_info->vs.param_exports = param_count;
}

static void
si_export_mrt_color(struct nir_to_llvm_context *ctx,
		    LLVMValueRef *color, unsigned index, bool is_last)
{
	LLVMValueRef args[9];
	/* Export */
	si_llvm_init_export_args(ctx, color, V_008DFC_SQ_EXP_MRT + index,
				 args);

	if (is_last) {
		args[1] = ctx->i32one; /* whether the EXEC mask is valid */
		args[2] = ctx->i32one; /* DONE bit */
	} else if (args[0] == ctx->i32zero)
		return; /* unnecessary NULL export */

	emit_llvm_intrinsic(ctx, "llvm.SI.export",
			    ctx->voidt, args, 9, 0);
}

static void
handle_fs_outputs_post(struct nir_to_llvm_context *ctx,
		       struct nir_shader *nir)
{
	unsigned index = 0;

	for (unsigned i = 0; i < RADEON_LLVM_MAX_OUTPUTS; ++i) {
		LLVMValueRef values[4];
		bool last;
		if (!(ctx->output_mask & (1ull << i)))
			continue;

		last = ctx->output_mask <= ((1ull << (i + 1)) - 1);

		for (unsigned j = 0; j < 4; j++)
			values[j] = to_float(ctx, LLVMBuildLoad(ctx->builder,
					      ctx->outputs[radeon_llvm_reg_index_soa(i, j)], ""));

		si_export_mrt_color(ctx, values, index, last);
		index++;
	}
}

static void
handle_shader_outputs_post(struct nir_to_llvm_context *ctx,
			   struct nir_shader *nir)
{
	switch (ctx->stage) {
	case MESA_SHADER_VERTEX:
		handle_vs_outputs_post(ctx, nir);
		break;
	case MESA_SHADER_FRAGMENT:
		handle_fs_outputs_post(ctx, nir);
		break;
	default:
		break;
	}
}

static void ac_llvm_finalize_module(struct nir_to_llvm_context * ctx)
{
	LLVMPassManagerRef passmgr;
	/* Create the pass manager */
	passmgr = LLVMCreateFunctionPassManagerForModule(
							ctx->module);

	/* This pass should eliminate all the load and store instructions */
	LLVMAddPromoteMemoryToRegisterPass(passmgr);

	/* Add some optimization passes */
	LLVMAddScalarReplAggregatesPass(passmgr);
	LLVMAddLICMPass(passmgr);
	LLVMAddAggressiveDCEPass(passmgr);
	LLVMAddCFGSimplificationPass(passmgr);
	LLVMAddInstructionCombiningPass(passmgr);

	/* Run the pass */
	LLVMRunFunctionPassManager(passmgr, ctx->main_function);

	LLVMDisposeBuilder(ctx->builder);
	LLVMDisposePassManager(passmgr);
}

static
LLVMModuleRef ac_translate_nir_to_llvm(LLVMTargetMachineRef tm,
                                       struct nir_shader *nir,
                                       struct ac_shader_variant_info *shader_info,
                                       const struct ac_nir_compiler_options *options)
{
	struct nir_to_llvm_context ctx = {0};
	struct nir_function *func;
	ctx.options = options;
	ctx.shader_info = shader_info;
	ctx.context = LLVMContextCreate();
	ctx.module = LLVMModuleCreateWithNameInContext("shader", ctx.context);

	memset(shader_info, 0, sizeof(*shader_info));

	LLVMSetTarget(ctx.module, "amdgcn--");
	setup_types(&ctx);

	const char *triple = LLVMGetTarget(ctx.module);

	ctx.builder = LLVMCreateBuilderInContext(ctx.context);
	ctx.stage = nir->stage;

	create_function(&ctx, nir);

	nir_foreach_variable(variable, &nir->inputs)
		handle_shader_input_decl(&ctx, variable);

	if (nir->stage == MESA_SHADER_FRAGMENT)
		handle_fs_inputs_pre(&ctx, nir);

	nir_foreach_variable(variable, &nir->outputs)
		handle_shader_output_decl(&ctx, variable);

	ctx.defs = _mesa_hash_table_create(NULL, _mesa_hash_pointer,
	                                   _mesa_key_pointer_equal);
	ctx.phis = _mesa_hash_table_create(NULL, _mesa_hash_pointer,
	                                   _mesa_key_pointer_equal);

	func = (struct nir_function *)exec_list_get_head(&nir->functions);

	setup_locals(&ctx, func);

	visit_cf_list(&ctx, &func->impl->body);
	phi_post_pass(&ctx);

	handle_shader_outputs_post(&ctx, nir);
	LLVMBuildRetVoid(ctx.builder);

	ac_llvm_finalize_module(&ctx);
	free(ctx.locals);
	ralloc_free(ctx.defs);
	ralloc_free(ctx.phis);

	return ctx.module;
}

static void ac_diagnostic_handler(LLVMDiagnosticInfoRef di, void *context)
{
	unsigned *retval = (unsigned *)context;
	LLVMDiagnosticSeverity severity = LLVMGetDiagInfoSeverity(di);
	char *description = LLVMGetDiagInfoDescription(di);

	if (severity == LLVMDSError) {
		*retval = 1;
		fprintf(stderr, "LLVM triggered Diagnostic Handler: %s\n",
		        description);
	}

	LLVMDisposeMessage(description);
}

static unsigned ac_llvm_compile(LLVMModuleRef M,
                                struct ac_shader_binary *binary,
                                LLVMTargetMachineRef tm)
{
	unsigned retval = 0;
	char *err;
	LLVMContextRef llvm_ctx;
	LLVMMemoryBufferRef out_buffer;
	unsigned buffer_size;
	const char *buffer_data;
	LLVMBool mem_err;

	/* Setup Diagnostic Handler*/
	llvm_ctx = LLVMGetModuleContext(M);

	LLVMContextSetDiagnosticHandler(llvm_ctx, ac_diagnostic_handler,
	                                &retval);

	/* Compile IR*/
	mem_err = LLVMTargetMachineEmitToMemoryBuffer(tm, M, LLVMObjectFile,
	                                              &err, &out_buffer);

	/* Process Errors/Warnings */
	if (mem_err) {
		fprintf(stderr, "%s: %s", __FUNCTION__, err);
		free(err);
		retval = 1;
		goto out;
	}

	/* Extract Shader Code*/
	buffer_size = LLVMGetBufferSize(out_buffer);
	buffer_data = LLVMGetBufferStart(out_buffer);

	ac_elf_read(buffer_data, buffer_size, binary);

	/* Clean up */
	LLVMDisposeMemoryBuffer(out_buffer);

out:
	return retval;
}

void ac_compile_nir_shader(LLVMTargetMachineRef tm,
                           struct ac_shader_binary *binary,
                           struct ac_shader_config *config,
                           struct ac_shader_variant_info *shader_info,
                           struct nir_shader *nir,
                           const struct ac_nir_compiler_options *options,
			   bool dump_shader)
{
	LLVMModuleRef llvm_module = ac_translate_nir_to_llvm(tm, nir, shader_info,
	                                                     options);
	if (dump_shader)
		LLVMDumpModule(llvm_module);

	memset(binary, 0, sizeof(*binary));
	int v = ac_llvm_compile(llvm_module, binary, tm);
	if (v) {
		fprintf(stderr, "compile failed\n");
	}

	if (dump_shader)
		fprintf(stderr, "disasm:\n%s\n", binary->disasm_string);

	ac_shader_binary_read_config(binary, config, 0);

	LLVMContextRef ctx = LLVMGetModuleContext(llvm_module);
	LLVMDisposeModule(llvm_module);
	LLVMContextDispose(ctx);

	if (nir->stage == MESA_SHADER_FRAGMENT) {
		shader_info->num_input_vgprs = 0;
		if (G_0286CC_PERSP_SAMPLE_ENA(config->spi_ps_input_addr))
			shader_info->num_input_vgprs += 2;
		if (G_0286CC_PERSP_CENTER_ENA(config->spi_ps_input_addr))
			shader_info->num_input_vgprs += 2;
		if (G_0286CC_PERSP_CENTROID_ENA(config->spi_ps_input_addr))
			shader_info->num_input_vgprs += 2;
		if (G_0286CC_PERSP_PULL_MODEL_ENA(config->spi_ps_input_addr))
			shader_info->num_input_vgprs += 3;
		if (G_0286CC_LINEAR_SAMPLE_ENA(config->spi_ps_input_addr))
			shader_info->num_input_vgprs += 2;
		if (G_0286CC_LINEAR_CENTER_ENA(config->spi_ps_input_addr))
			shader_info->num_input_vgprs += 2;
		if (G_0286CC_LINEAR_CENTROID_ENA(config->spi_ps_input_addr))
			shader_info->num_input_vgprs += 2;
		if (G_0286CC_LINE_STIPPLE_TEX_ENA(config->spi_ps_input_addr))
			shader_info->num_input_vgprs += 1;
		if (G_0286CC_POS_X_FLOAT_ENA(config->spi_ps_input_addr))
			shader_info->num_input_vgprs += 1;
		if (G_0286CC_POS_Y_FLOAT_ENA(config->spi_ps_input_addr))
			shader_info->num_input_vgprs += 1;
		if (G_0286CC_POS_Z_FLOAT_ENA(config->spi_ps_input_addr))
			shader_info->num_input_vgprs += 1;
		if (G_0286CC_POS_W_FLOAT_ENA(config->spi_ps_input_addr))
			shader_info->num_input_vgprs += 1;
		if (G_0286CC_FRONT_FACE_ENA(config->spi_ps_input_addr))
			shader_info->num_input_vgprs += 1;
		if (G_0286CC_ANCILLARY_ENA(config->spi_ps_input_addr))
			shader_info->num_input_vgprs += 1;
		if (G_0286CC_SAMPLE_COVERAGE_ENA(config->spi_ps_input_addr))
			shader_info->num_input_vgprs += 1;
		if (G_0286CC_POS_FIXED_PT_ENA(config->spi_ps_input_addr))
			shader_info->num_input_vgprs += 1;
	}
	config->num_vgprs = MAX2(config->num_vgprs, shader_info->num_input_vgprs);

	/* +3 for scratch wave offset and VCC */
	config->num_sgprs = MAX2(config->num_sgprs,
	                         shader_info->num_input_sgprs + 3);
}
