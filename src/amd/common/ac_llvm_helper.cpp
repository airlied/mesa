/*
 * Copyright 2014 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 */

/* based on Marek's patch to lp_bld_misc.cpp */

// Workaround http://llvm.org/PR23628
#pragma push_macro("DEBUG")
#undef DEBUG

#include "ac_llvm_util.h"
#include <llvm-c/Core.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/IR/LegacyPassManager.h>

#include <llvm-c/Transforms/IPO.h>
#include <llvm-c/Transforms/Scalar.h>
#if HAVE_LLVM >= 0x0700
#include <llvm-c/Transforms/Utils.h>
#endif

#if HAVE_LLVM < 0x0700
#include "llvm/Support/raw_ostream.h"
#endif
#include <list>

void ac_add_attr_dereferenceable(LLVMValueRef val, uint64_t bytes)
{
   llvm::Argument *A = llvm::unwrap<llvm::Argument>(val);
   A->addAttr(llvm::Attribute::getWithDereferenceableBytes(A->getContext(), bytes));
}

bool ac_is_sgpr_param(LLVMValueRef arg)
{
	llvm::Argument *A = llvm::unwrap<llvm::Argument>(arg);
	llvm::AttributeList AS = A->getParent()->getAttributes();
	unsigned ArgNo = A->getArgNo();
	return AS.hasAttribute(ArgNo + 1, llvm::Attribute::InReg);
}

LLVMValueRef ac_llvm_get_called_value(LLVMValueRef call)
{
	return LLVMGetCalledValue(call);
}

bool ac_llvm_is_function(LLVMValueRef v)
{
	return LLVMGetValueKind(v) == LLVMFunctionValueKind;
}

LLVMBuilderRef ac_create_builder(LLVMContextRef ctx,
				 enum ac_float_mode float_mode)
{
	LLVMBuilderRef builder = LLVMCreateBuilderInContext(ctx);

	llvm::FastMathFlags flags;

	switch (float_mode) {
	case AC_FLOAT_MODE_DEFAULT:
		break;
	case AC_FLOAT_MODE_NO_SIGNED_ZEROS_FP_MATH:
		flags.setNoSignedZeros();
		llvm::unwrap(builder)->setFastMathFlags(flags);
		break;
	case AC_FLOAT_MODE_UNSAFE_FP_MATH:
#if HAVE_LLVM >= 0x0600
		flags.setFast();
#else
		flags.setUnsafeAlgebra();
#endif
		llvm::unwrap(builder)->setFastMathFlags(flags);
		break;
	}

	return builder;
}

LLVMTargetLibraryInfoRef
ac_create_target_library_info(const char *triple)
{
	return reinterpret_cast<LLVMTargetLibraryInfoRef>(new llvm::TargetLibraryInfoImpl(llvm::Triple(triple)));
}

void
ac_dispose_target_library_info(LLVMTargetLibraryInfoRef library_info)
{
	delete reinterpret_cast<llvm::TargetLibraryInfoImpl *>(library_info);
}

class ac_llvm_per_thread_info {
public:
	ac_llvm_per_thread_info(enum radeon_family arg_family,
				enum ac_target_machine_options arg_tm_options)
		: family(arg_family), tm_options(arg_tm_options),
		  OStream(CodeString) {}
	~ac_llvm_per_thread_info() {
		ac_llvm_compiler_dispose_internal(&llvm_info);
	}

	struct ac_llvm_compiler_info llvm_info;
	enum radeon_family family;
	enum ac_target_machine_options tm_options;
	llvm::SmallString<0> CodeString;
	llvm::raw_svector_ostream OStream;
	llvm::legacy::PassManager pass;
};

/* we have to store a linked list per thread due to the possiblity of multiple gpus being required */
static thread_local std::list<ac_llvm_per_thread_info> ac_llvm_per_thread_list;

bool ac_compile_to_memory_buffer(struct ac_llvm_compiler_info *info,
				 LLVMModuleRef M,
				 char **ErrorMessage,
				 LLVMMemoryBufferRef *OutMemBuf)
{
	ac_llvm_per_thread_info *thread_info = nullptr;
	if (info->thread_stored) {
		for (auto &I : ac_llvm_per_thread_list) {
			if (I.llvm_info.tm == info->tm) {
				thread_info = &I;
				break;
			}
		}

		if (!thread_info) {
			assert(0);
			return false;
		}
	} else {
		return LLVMTargetMachineEmitToMemoryBuffer(info->tm, M, LLVMObjectFile,
							   ErrorMessage, OutMemBuf);
	}

	llvm::TargetMachine *TM = reinterpret_cast<llvm::TargetMachine*>(thread_info->llvm_info.tm);
	llvm::Module *Mod = llvm::unwrap(M);
	llvm::StringRef Data;

	Mod->setDataLayout(TM->createDataLayout());

	thread_info->pass.run(*Mod);

	Data = thread_info->OStream.str();
	*OutMemBuf = LLVMCreateMemoryBufferWithMemoryRangeCopy(Data.data(), Data.size(), "");
	thread_info->CodeString = "";
	return false;
}

bool ac_llvm_compiler_init(struct ac_llvm_compiler_info *info,
			   bool add_target_library_info,
			   enum radeon_family family,
			   enum ac_target_machine_options tm_options)
{
	if (tm_options & AC_TM_THREAD_LLVM) {
		for (auto &I : ac_llvm_per_thread_list) {
			if (I.family == family &&
			    I.tm_options == tm_options) {
				*info = I.llvm_info;
				return true;
			}
		}

		ac_llvm_per_thread_list.emplace_back(family, tm_options);
		ac_llvm_per_thread_info &tinfo = ac_llvm_per_thread_list.back();
		if (!ac_llvm_compiler_init_internal(&tinfo.llvm_info,
						    true,
						    family,
						    tm_options))
			return false;

		tinfo.llvm_info.thread_stored = true;
		*info = tinfo.llvm_info;

		llvm::TargetMachine *TM = reinterpret_cast<llvm::TargetMachine*>(tinfo.llvm_info.tm);
		if (TM->addPassesToEmitFile(tinfo.pass, tinfo.OStream,
#if HAVE_LLVM >= 0x0700
					    nullptr,
#endif
					llvm::TargetMachine::CGFT_ObjectFile)) {
			assert(0);
			return false;
		}
	} else {
		if (!ac_llvm_compiler_init_internal(info,
						    add_target_library_info,
						    family,
						    tm_options))
			return false;
	}
	return true;
}

void ac_llvm_compiler_dispose(struct ac_llvm_compiler_info *info)
{
	if (!info->thread_stored)
		ac_llvm_compiler_dispose_internal(info);
}
