#pragma once

#include <llvm-c/TargetMachine.h>

#include "ac_radeon_winsys.h"

LLVMTargetMachineRef ac_create_target_machine(enum radeon_family family);
