
// Workaround http://llvm.org/PR23628
#if HAVE_LLVM >= 0x0307
#  pragma push_macro("DEBUG")
#  undef DEBUG
#endif

#include "ac_nir_to_llvm.h"
#include <llvm-c/Core.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>

extern "C" void
ac_add_attr_dereferenceable(LLVMValueRef val, uint64_t bytes)
{
#if HAVE_LLVM >= 0x0306
   llvm::Argument *A = llvm::unwrap<llvm::Argument>(val);
   llvm::AttrBuilder B;
   B.addDereferenceableAttr(bytes);
   A->addAttr(llvm::AttributeSet::get(A->getContext(), A->getArgNo() + 1,  B));
#endif
}
