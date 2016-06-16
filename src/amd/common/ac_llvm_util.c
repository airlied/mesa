#include "ac_llvm_util.h"

#include <llvm-c/Core.h>

#include "c11/threads.h"

#include <assert.h>
#include <stdio.h>

static void ac_init_llvm_target()
{
#if HAVE_LLVM < 0x0307
	LLVMInitializeR600TargetInfo();
	LLVMInitializeR600Target();
	LLVMInitializeR600TargetMC();
	LLVMInitializeR600AsmPrinter();
#else
	LLVMInitializeAMDGPUTargetInfo();
	LLVMInitializeAMDGPUTarget();
	LLVMInitializeAMDGPUTargetMC();
	LLVMInitializeAMDGPUAsmPrinter();
#endif
}

static once_flag ac_init_llvm_target_once_flag = ONCE_FLAG_INIT;

static LLVMTargetRef ac_get_llvm_target(const char *triple)
{
	LLVMTargetRef target = NULL;
	char *err_message = NULL;

	call_once(&ac_init_llvm_target_once_flag, ac_init_llvm_target);

	if (LLVMGetTargetFromTriple(triple, &target, &err_message)) {
		fprintf(stderr, "Cannot find target for triple %s ", triple);
		if (err_message) {
			fprintf(stderr, "%s\n", err_message);
		}
		LLVMDisposeMessage(err_message);
		return NULL;
	}
	return target;
}

static const char *ac_get_llvm_processor_name(enum radeon_family family)
{
	switch (family) {
	case CHIP_TAHITI:
		return "tahiti";
	case CHIP_PITCAIRN:
		return "pitcairn";
	case CHIP_VERDE:
		return "verde";
	case CHIP_OLAND:
		return "oland";
	case CHIP_HAINAN:
		return "hainan";
	case CHIP_BONAIRE:
		return "bonaire";
	case CHIP_KABINI:
		return "kabini";
	case CHIP_KAVERI:
		return "kaveri";
	case CHIP_HAWAII:
		return "hawaii";
	case CHIP_MULLINS:
		return "mullins";
	case CHIP_TONGA:
		return "tonga";
	case CHIP_ICELAND:
		return "iceland";
	case CHIP_CARRIZO:
		return "carrizo";
#if HAVE_LLVM <= 0x0307
	case CHIP_FIJI:
		return "tonga";
	case CHIP_STONEY:
		return "carrizo";
#else
	case CHIP_FIJI:
		return "fiji";
	case CHIP_STONEY:
		return "stoney";
#endif
#if HAVE_LLVM <= 0x0308
	case CHIP_POLARIS10:
		return "tonga";
	case CHIP_POLARIS11:
		return "tonga";
#else
	case CHIP_POLARIS10:
		return "polaris10";
	case CHIP_POLARIS11:
		return "polaris11";
#endif
	default:
		return "";
	}
}

LLVMTargetMachineRef ac_create_target_machine(enum radeon_family family)
{
	assert(family >= CHIP_TAHITI);

	const char *triple = "amdgcn--";
	LLVMTargetRef target = ac_get_llvm_target(triple);
	LLVMTargetMachineRef tm = LLVMCreateTargetMachine(
	                             target,
	                             triple,
	                             ac_get_llvm_processor_name(family),
	                             "+DumpCode,+vgpr-spilling",
	                             LLVMCodeGenLevelDefault,
	                             LLVMRelocDefault,
	                             LLVMCodeModelDefault);

	return tm;
}
