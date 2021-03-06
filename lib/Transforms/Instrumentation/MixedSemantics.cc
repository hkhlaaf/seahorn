#include "seahorn/Transforms/Instrumentation/MixedSemantics.hh"
#include "seahorn/Analysis/CanFail.hh"
#include "seahorn/Transforms/Scalar/PromoteVerifierCalls.hh"
#include "seahorn/Transforms/Utils/Local.hh"

#include "llvm/Analysis/CallGraph.h"
#include "llvm/ADT/SCCIterator.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/IR/CFG.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/IR/GlobalValue.h"

#include "boost/range.hpp"

static llvm::cl::opt<bool>
ReduceMain("ms-reduce-main",
             llvm::cl::desc ("Reduce main to return paths"),
             llvm::cl::init (false));

namespace seahorn
{
  using namespace llvm;

  char MixedSemantics::ID = 0;
  
  static void removeError (Function &F)
  {
    for (auto &BB : F)
    {
      for (auto &I : BB)
      {
        CallInst *ci = dyn_cast<CallInst> (&I);
        if (!ci) continue;
        Function* cf = ci->getCalledFunction ();
        if (!cf) continue;
        if (!cf->getName ().equals ("verifier.error")) continue;
        
        auto assumeFn = 
          F.getParent ()->getOrInsertFunction ("verifier.assume", 
                                                 Type::getVoidTy (F.getContext ()), 
                                                 Type::getInt1Ty (F.getContext ()), NULL);
        ReplaceInstWithInst (ci, 
                             CallInst::Create (assumeFn, 
                                               ConstantInt::getFalse (F.getContext ())));
        // does not matter what verifier.error() call is followed by in this bb
        break;
      }
    }
    
  }


  static bool ExternalizeDeclarations (Module &M)
  {
    bool change = false;
    for (Module::iterator I = M.begin(), E = M.end(); I != E; ) 
    {
      Function *F = I++;
      if (F->isDeclaration() &&
          (F->getLinkage () == GlobalValue::LinkageTypes::InternalLinkage))
      {
        F->setLinkage (GlobalValue::LinkageTypes::ExternalLinkage);
        change = true;
      }
    }
    return change;
  }
 
  bool MixedSemantics::runOnModule (Module &M)
  {
    Function *main = M.getFunction ("main");
    if (!main) return false;
    
    CanFail &CF = getAnalysis<CanFail> ();
    if (!CF.canFail (main)) 
    {
      // -- this benefits the legacy front-end. 
      // -- it might create issues in some applications where mixed-semantics is applied
      if (ReduceMain) reduceToReturnPaths (*main);
      return false;
    }
    
    
    main->setName ("orig.main");
    FunctionType *mainTy = main->getFunctionType ();
    FunctionType *newTy = 
      FunctionType::get (Type::getInt32Ty (M.getContext ()),
                         ArrayRef<Type*> (mainTy->param_begin (),
                                          mainTy->param_end ()), false);
    
    Function *newM = Function::Create (newTy,
                                       main->getLinkage (),
                                       "main", &M);
    newM->copyAttributesFrom (main);

    // -- mark old main as private
    main->setLinkage (GlobalValue::LinkageTypes::PrivateLinkage);
    
    BasicBlock *entry = BasicBlock::Create (M.getContext (),
                                            "entry", newM);
    
    IRBuilder<> Builder (M.getContext ());
    Builder.SetInsertPoint (entry, entry->begin ());
    
    
    SmallVector<Value*,16> fargs;
    for (auto &a : boost::make_iterator_range (newM->arg_begin (), 
                                               newM->arg_end ()))
      fargs.push_back (&a);
    
    InlineFunctionInfo IFI;
    CallInst *mcall = Builder.CreateCall (main, fargs);
    Builder.CreateUnreachable ();
    InlineFunction (mcall, IFI);

    DenseMap<const Function*, BasicBlock*> entryBlocks;
    DenseMap<const Function*, SmallVector<Value*, 16> > entryPrms;
    
    SmallVector<const BasicBlock*, 4> errBlocks;
    
    
    IRBuilder<> enBldr (M.getContext ());
    enBldr.SetInsertPoint (entry, entry->begin ());
    
    for (Function &F : M)
    {
      if (&F == main || &F == newM) continue;
      if (!CF.canFail (&F)) 
      {
        if (!F.isDeclaration ()) reduceToReturnPaths (F);
        continue;
      }
      
      BasicBlock *bb = BasicBlock::Create (M.getContext (), F.getName (), newM);
      entryBlocks [&F] = bb;
      
      Builder.SetInsertPoint (bb, bb->begin ());
      
      if (!CF.mustFail (&F)) 
      {
        fargs.clear ();
        auto &p = entryPrms [&F];
        for (auto &a : boost::make_iterator_range (F.arg_begin (), F.arg_end ()))
        {
          p.push_back (enBldr.CreateAlloca (a.getType ()));
          fargs.push_back (Builder.CreateLoad (p.back ()));
        }
    
        CallInst *fcall = Builder.CreateCall (&F, fargs);
        Builder.CreateUnreachable ();
        InlineFunction (fcall, IFI);
        removeError (F);
        reduceToReturnPaths (F);
      }
      else
      {
        Builder.CreateRet (Builder.getInt32 (42));
        errBlocks.push_back (bb);
      }
      
    }
    
    std::vector<CallInst*> workList;
    
    Constant *ndFn = M.getOrInsertFunction ("nondet.bool", 
                                            Type::getInt1Ty (M.getContext ()), NULL);
    for (BasicBlock &BB : *newM)
    {
      for (auto &I : BB)
      {
        CallInst *ci = dyn_cast<CallInst> (&I);
        if (!ci) continue;
        Function *cf = ci->getCalledFunction ();
        if (entryBlocks.count (cf) <= 0) continue;
        // -- would create a back-jump
        if (entryBlocks [cf] == &BB) continue;
        workList.push_back (ci);
      }
    }
    
    for (auto *ci : workList)
    {
      BasicBlock *bb = ci->getParent ();
      BasicBlock *post = bb->splitBasicBlock (ci, "postcall");
      
      bb->getTerminator ()->eraseFromParent ();
      Builder.SetInsertPoint (bb);
      
      if (CF.mustFail (ci->getCalledFunction ())) 
      {
        Builder.CreateBr (entryBlocks [ci->getCalledFunction ()]);
        continue;
      }
      
      BasicBlock *argBb = 
        BasicBlock::Create (M.getContext (), "precall", newM, post);
      Builder.CreateCondBr (Builder.CreateCall (ndFn),
                            post, argBb);
      
      Builder.SetInsertPoint (argBb);
      
      CallSite CS (ci);
      auto &params = entryPrms[ci->getCalledFunction ()];
      
      for (unsigned i = 0; i < params.size (); ++i)
        Builder.CreateStore (CS.getArgument (i), params[i]);
      
      Builder.CreateBr (entryBlocks [ci->getCalledFunction ()]);
    }
    
    reduceToAncestors (*newM, errBlocks);
    ExternalizeDeclarations (M);

    return true;
  }
  
  void MixedSemantics::getAnalysisUsage (AnalysisUsage &AU) const
  {
    AU.addRequiredID (LowerSwitchID);
    AU.addRequired<CallGraphWrapperPass> ();
    AU.addRequired<CanFail> ();
    AU.addRequired<PromoteVerifierCalls> ();
  }
  
}

static llvm::RegisterPass<seahorn::MixedSemantics>
X ("mixed-semantics", "Transform into mixed semantics");
