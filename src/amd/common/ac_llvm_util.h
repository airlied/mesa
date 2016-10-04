#pragma once

#include <llvm-c/TargetMachine.h>

#include "amd_family.h"

LLVMTargetMachineRef ac_create_target_machine(enum radeon_family family);
