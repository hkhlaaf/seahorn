#include "seahorn/HornifyModule.hh"


#include "ufo/Passes/NameValues.hpp"

#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/Transforms/Utils/UnifyFunctionExitNodes.h"
#include "llvm/IR/CFG.h"
#include "llvm/Analysis/CFG.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/ADT/SCCIterator.h"
#include "seahorn/Support/BoostLlvmGraphTraits.hh"

#include "boost/range.hpp"
#include "boost/scoped_ptr.hpp"

#include "seahorn/Support/SortTopo.hh"

#include "seahorn/SymStore.hh"
#include "seahorn/LiveSymbols.hh"

#include "seahorn/Analysis/CutPointGraph.hh"
#include "seahorn/Analysis/CanFail.hh"
#include "ufo/Smt/EZ3.hh"
#include "ufo/Stats.hh"

#include "seahorn/HornifyFunction.hh"
#include "seahorn/FlatHornifyFunction.hh"

using namespace llvm;
using namespace seahorn;


static llvm::cl::opt<enum TrackLevel>
TL("horn-sem-lvl",
   llvm::cl::desc ("Track level for symbolic execution"),
   cl::values (clEnumValN (REG, "reg", "Primitive registers only"),
               clEnumValN (PTR, "ptr", "REG + pointers"),
               clEnumValN (MEM, "mem", "PTR + memory content"),
               clEnumValEnd),
   cl::init (seahorn::REG));


namespace hm_detail {enum Step {SMALL_STEP, LARGE_STEP, CLP_SMALL_STEP, FLAT_LARGE_STEP};}

static llvm::cl::opt<enum hm_detail::Step>
Step("horn-step",
     llvm::cl::desc ("Step to use for the encoding"),
     cl::values (clEnumValN (hm_detail::SMALL_STEP, "small", "Small Step"),
                 clEnumValN (hm_detail::LARGE_STEP, "large", "Large Step"),
                 clEnumValN (hm_detail::FLAT_LARGE_STEP, "flarge", "Flat Large Step"),
                 clEnumValN (hm_detail::CLP_SMALL_STEP, "clpsmall", "CLP Small Step"),
                 clEnumValEnd),
     cl::init (hm_detail::SMALL_STEP));

static llvm::cl::opt<bool>
InterProc("horn-inter-proc",
          llvm::cl::desc ("Use inter-procedural encoding"),
          cl::init (false));

namespace seahorn
{
  char HornifyModule::ID = 0;

  HornifyModule::HornifyModule () :
    ModulePass (ID), m_zctx (m_efac),  m_db (m_efac),
    m_td(0)
  {
  }

  bool HornifyModule::runOnModule (Module &M)
  {
    ScopedStats _st ("HornifyModule");

    bool Changed = false;
    m_td = &getAnalysis<DataLayoutPass> ().getDataLayout ();

    if (Step == hm_detail::CLP_SMALL_STEP)
      m_sem.reset (new ClpSmallSymExec (m_efac, *this, TL));
    else
      m_sem.reset (new UfoSmallSymExec (m_efac, *this, TL));

    // create FunctionInfo for verifier.error() function
    if (Function* errorFn = M.getFunction ("verifier.error"))
    {
      FunctionInfo &fi = m_sem->getFunctionInfo (*errorFn);
      Expr boolSort = sort::boolTy (m_efac);
      ExprVector sorts (4, boolSort);
      fi.sumPred = bind::fdecl (mkTerm<const Function*> (errorFn, m_efac), sorts);
      m_db.registerRelation (fi.sumPred);

      // basic rules for error
      // error (false, false, false)
      // error (false, true, true)
      // error (true, false, true)
      // error (true, true, true)

      Expr trueE = mk<TRUE> (m_efac);
      Expr falseE = mk<FALSE> (m_efac);

      ExprSet allVars;

      ExprVector args {falseE, falseE, falseE};
      m_db.addRule (allVars, bind::fapp (fi.sumPred, args));

      args = {falseE, trueE, trueE} ;
      m_db.addRule (allVars, bind::fapp (fi.sumPred, args));

      args = {trueE, falseE, trueE} ;
      m_db.addRule (allVars, bind::fapp (fi.sumPred, args));

      args = {trueE, trueE, trueE} ;
      m_db.addRule (allVars, bind::fapp (fi.sumPred, args));

      args [0] = bind::boolConst (mkTerm (std::string ("arg.0"), m_efac));
      args [1] = bind::boolConst (mkTerm (std::string ("arg.1"), m_efac));
      args [2] = bind::boolConst (mkTerm (std::string ("arg.2"), m_efac));
      m_db.addConstraint (bind::fapp (fi.sumPred, args),
                          mk<AND> (mk<OR> (mk<NEG> (args [0]), args [2]),
                                   mk<OR> (args [0], mk<EQ> (args [1], args [2]))));
    }


    CallGraph &CG = getAnalysis<CallGraphWrapperPass> ().getCallGraph ();
    for (auto it = scc_begin (&CG); !it.isAtEnd (); ++it)
    {
      const std::vector<CallGraphNode*> &scc = *it;
      CallGraphNode *cgn = scc.front ();
      Function *f = cgn->getFunction ();
      if (it.hasLoop () || scc.size () > 1)
        errs () << "WARNING RECURSION at " << (f ? f->getName () : "nil") << "\n";
      // assert (!it.hasLoop () && "Recursion not yet supported");
      // assert (scc.size () == 1 && "Recursion not supported");
      if (f) Changed = (runOnFunction (*f) || Changed);
    }


    /**
       TODO:
         - name basic blocks so that there are no name clashes between functions (DONE)
         - handle new shadow mem functions in SymExec
         - add attributes (i.e., does not read memory) to shadow mem functions
         - factor out Live analysis from SymExec.
       To implement:

       1. walk up the call graph and compute live symbols

       2. from entry block of each function, compute globals that the
          function is using, and make it available to live analysis

       3. live analysis reads all globals that the function is using at a call site

       4. for non-trivial call graph scc

          a. run live analysis on each function
          b. merge live globals from all functions
          c. run one step of live analysis again with new global usage info

      At this point global liveness information is done

       5. update live values of entry blocks to include DSNodes that
          are passed in. This information is available from annotations
          in the return block left by the MemShadowDsaPass

       8. create summary predicates for functions. Use llvm::Function* as the name.
          Ensure that BasicBlocks and CutPoint include function in their name.
          Signature: live@entry (non-memory) , return val, memory regions

       7. use HornifyFunction to compute the system for each
          individual function Give it access to summary predicates so
          that it can deal with functions.  It will need to map
          parameters to arguments and globals to match live@entry part
          of the signature.

       8. Add rules connecting summaries. The rules are from return block.
           return_block(live@entry, live@other) & ret_block_actions ->
                           Summary (live@entry (non-memory), ret_val, memory)

       9. main is special:

            a. live at entry are globals and global memory
            b. entry rule is the initialization (i.e., all global values are 0 initially)
            c. query is whether main gets to its return location (same as UFO)

    */
    return Changed;
  }

  bool HornifyModule::runOnFunction (Function &F)
  {
    // -- skip functions without a body
    if (F.isDeclaration () || F.empty ()) return false;
    LOG("horn-step", errs () << "HornifyModule: runOnFunction: " << F.getName () << "\n");



    //CutPointGraph &cpg = getAnalysis<CutPointGraph> (F);
    boost::scoped_ptr<HornifyFunction> hf (new SmallHornifyFunction
                                           (*this, InterProc));
    if (Step == hm_detail::LARGE_STEP)
      hf.reset (new LargeHornifyFunction (*this, InterProc));
    else if (Step == hm_detail::FLAT_LARGE_STEP)
      hf.reset (new FlatLargeHornifyFunction (*this, InterProc));


    /// -- allocate LiveSymbols
    auto r = m_ls.insert (std::make_pair (&F, LiveSymbols (F, m_efac, *m_sem)));
    assert (r.second);
    /// -- run LiveSymbols
    r.first->second.run ();

    /// -- hornify function
    hf->runOnFunction (F);

    return false;
  }

  void HornifyModule::getAnalysisUsage (llvm::AnalysisUsage &AU) const
  {
    AU.setPreservesAll ();
    AU.addRequired<llvm::DataLayoutPass>();

    AU.addRequired<seahorn::CanFail> ();
    AU.addRequired<ufo::NameValues>();

    AU.addRequired<llvm::CallGraphWrapperPass> ();
    AU.addPreserved<llvm::CallGraphWrapperPass> ();

    AU.addRequired<seahorn::TopologicalOrder>();
    AU.addRequired<seahorn::CutPointGraph>();

  }

  const LiveSymbols& HornifyModule::getLiveSybols (const Function &F) const
  {
    auto it = m_ls.find (&F);
    assert (it != m_ls.end ());
    return it->second;
  }

  const Expr HornifyModule::bbPredicate (const BasicBlock &BB)
  {
    const BasicBlock *bb = &BB;
    Expr res = m_bbPreds [bb];
    if (res) return res;


    const ExprVector &lv = live (bb);
    ExprVector sorts;
    sorts.reserve (lv.size () + 1);

    for (auto &v : lv)
    {
      assert (bind::isFapp (v));
      assert (bind::domainSz (bind::fname (v)) == 0);
      sorts.push_back (bind::typeOf (v));
    }
    sorts.push_back (mk<BOOL_TY> (m_efac));

    Expr name = mkTerm (bb, m_efac);
    res = bind::fdecl (name, sorts);
    m_bbPreds [bb] = res;
    return res;
  }

  const BasicBlock& HornifyModule::predicateBb (Expr pred) const
  {
    Expr v = pred;
    if (bind::isFapp (v)) v = bind::fname (pred);

    assert (bind::isFdecl (v));
    v = bind::fname (v);
    assert (isOpX<BB> (v));
    return *getTerm<const BasicBlock*> (v);
  }

}
