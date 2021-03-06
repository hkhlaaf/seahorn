#ifndef SEAHORN_PASSES__HH_
#define SEAHORN_PASSES__HH_

#include "seahorn/config.h"
#include "llvm/Pass.h"
namespace seahorn
{
  llvm::Pass* createMarkInternalInlinePass ();
  llvm::Pass* createNondetInitPass ();
  llvm::Pass* createDeadNondetElimPass ();
  llvm::Pass* createDummyExitBlockPass ();

  llvm::Pass* createLoadIkosPass ();
  llvm::Pass* createShadowMemDsaPass ();

  llvm::Pass* createCutLoopsPass ();
}

#ifdef HAVE_LLVM_SEAHORN
#include "llvm_seahorn/Transforms/Scalar.h"
namespace seahorn
{
  inline llvm::FunctionPass* createInstCombine ()
  {return llvm_seahorn::createInstructionCombiningPass ();}
}
#else
#include "llvm/Transforms/Scalar.h"
namespace seahorn
{
  inline llvm::FunctionPass* createInstCombine()
  {return llvm::createInstructionCombiningPass ();}
}
#endif

#endif /* SEAHORN_PASSES__HH_ */
