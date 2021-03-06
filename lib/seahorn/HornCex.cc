#include "seahorn/HornCex.hh"

#include "llvm/IR/Function.h"
#include "ufo/Stats.hh"

#include "boost/range/algorithm/reverse.hpp"

#include "seahorn/HornifyModule.hh"
#include "seahorn/HornSolver.hh"
#include "seahorn/Analysis/CutPointGraph.hh"
#include "seahorn/Analysis/CanFail.hh"

#include "boost/range.hpp"
#include "boost/range/adaptor/reversed.hpp"
#include "boost/range/algorithm/sort.hpp"
#include "boost/container/flat_set.hpp"
#include <boost/algorithm/string/predicate.hpp>

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/FileSystem.h"

#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/DebugInfo.h"

static llvm::cl::opt<std::string>
SvCompCexFile("horn-svcomp-cex", llvm::cl::desc("Counterexample in SV-COMP XML format"),
              llvm::cl::init(""), llvm::cl::value_desc("filename"));

using namespace llvm;
namespace seahorn
{
  
  /// trivial an inefficient simplifier
  static void simplify (ExprFactory &efac, ExprVector &vec, ExprMap &side)
  {
    Expr trueE = mk<TRUE> (efac);
    Expr falseE = mk<FALSE> (efac);
    
    bool changed = true;
    
    errs () << "In simplify\n";
    
    while (changed)
    {
      changed = false;
      for (unsigned i = 0; i < vec.size (); ++i)
      {
        Expr &v = vec [i];
        
        if (isOpX<TRUE> (v)) continue;
        
        Expr u = replaceSimplify (v, side);
        assert (u.get ());
        if (u != v) 
        {
          v = u;
          changed = true;
        }
        
        if (isOpX<FALSE> (v))
        {
          errs () << "Got false\n";
          errs ().flush ();
          
          vec.clear ();
          vec.push_back (falseE);
          return;
        }
        else if (bind::isBoolConst (v))
        {
          side[v] = trueE;
          v = trueE;
          changed = true;
        }
        else if (isOpX<NEG> (v) && bind::isBoolConst (v->arg (0)))
        {
          side [v->arg (0)] = falseE;
          v = trueE;
          changed = true;
        }
        else if (isOpX<EQ> (v) || isOpX<IFF> (v))
        {
          if (v-> arg (0) != v->arg (1))
            side [v->arg (0)] = v->arg (1);
          v = trueE;
          changed = true;
        }
        else if (isOpX<AND> (v))
          {
            u = v;
            v = u->arg (0);
            // -- split and
            vec.insert (vec.end(), ++u->args_begin (), u->args_end ());
            changed = true;
          }
      }
    }
    
    for (unsigned i = 0; i < vec.size ();)
    {
      if (isOpX<TRUE> (vec [i]))
      {
        vec[i] = vec.back ();
        vec.pop_back ();
      }
      else ++i;
    }
    
    LOG ("cex_simp", 
         errs () << "side after simplification\n";
         for (auto &kv : side)
           errs () << *kv.first << " == " << *kv.second << "\n";);
  }
  
      
  
  template <typename O>
  class SvCompCex
  {
    O &m_out;
    unsigned m_id;
    
    void key (std::string name, std::string type, std::string obj, std::string id)
    {
      m_out << "<key attr.name='" << name << "' attr.type='" << type << "'"
            << " for='" << obj << "' id='" << id << "'/>\n";
    }
    
  public:
    SvCompCex (O &out) : m_out (out), m_id(0) {}
    void header ()
    {
      m_out << "<graphml xmlns:xsi='http://www.w3.org/2001/XMLSchema-instance' "
            << "xmlns='http://graphml.graphdrawing.org/xmlns'>\n";
      key ("sourcecodeLanguage", "string", "graph", "sourcecodelang");
      key ("lineNumberInOrigin", "int", "edge", "originline");
      key ("originFileName", "string", "edge", "originfile");
      key ("isEntryNode", "boolean", "node", "entry");
      key ("isSinkNode", "boolean", "node", "sink");
      key ("enterFunction", "string", "edge", "enterFunction");
      key ("returnFromFunction", "string", "edge", "returnFrom");
      
      m_out << "<graph edgedefault='directed'>\n"
            << "<data key='sourcecodelang'>C</data>\n"
            << "<node id='0'> <data key='entry'>true</data> </node>\n";
    }
    
    void edge (std::string file, int lineno, std::string scope)
    {
      unsigned src = m_id++;
      m_out << "<node id='" << m_id << "'/>\n";
      m_out << "<edge source='" << src << "' target='" << m_id << "'>\n";
      m_out << "  <data key='originline'>" << lineno << "</data>\n";
      m_out << "  <data key='originfile'>" << file << "</data>\n";

      if (boost::starts_with (scope, "enter: "))
        m_out << "  <data key='enterFunction'>" 
              << scope.substr (std::string ("enter: ").size ())
              << "</data>\n";
      else if (boost::starts_with (scope, "exit: "))
        m_out << "  <data key='returnFrom'>" 
              << scope.substr (std::string ("exit: ").size ())
              << "</data>\n";
      
      m_out << "</edge>\n";

    }
    
    void footer ()
    {
      m_out << "</graph></graphml>\n";
    }
  };
    
  
  char HornCex::ID = 0;
  
  bool HornCex::runOnModule (Module &M)
  {
    for (Function &F : M)
      if (F.getName ().equals ("main")) return runOnFunction (F);
    return false;
  }
  
  static std::string constAsString (const ConstantExpr *gep)
  {
    assert (gep != NULL);
    assert (gep->isGEPWithNoNotionalOverIndexing ());
    
    const GlobalVariable *gv = 
      dyn_cast<const GlobalVariable> (gep->getOperand (0));
    assert (gv != NULL);
    assert (gv->hasInitializer ());

    const ConstantDataSequential *op = 
      dyn_cast<const ConstantDataSequential> (gv->getInitializer ());
    assert (op != NULL);
    assert (op->isCString ());
    return op->getAsString ();
 }

  template <typename O>
  static void printLiner (const CallInst &ci, 
                          SvCompCex<O> &svcomp)
  {
    const ConstantInt *line = 
      dyn_cast<const ConstantInt> (ci.getArgOperand (0));
    assert (line != NULL);
		
    const ConstantExpr *file = 
      dyn_cast<const ConstantExpr> (ci.getArgOperand (1));
    assert (file != NULL);
		
    const ConstantExpr *scope = 
      dyn_cast<const ConstantExpr> (ci.getArgOperand (2));
    assert (scope != NULL);
		

    // -- use c_str() to hide the trailing 0 char
    // errs () << constAsString (file).c_str () << ":" 
    //         << line->getSExtValue () << ":"
    //         << constAsString (scope).c_str () << "\n";   
    svcomp.edge (constAsString (file).c_str (),
                 line->getSExtValue (), 
                 constAsString (scope).c_str ());
  }

  template <typename O>
  static void printDebugLoc (const DebugLoc& dloc,
                             SvCompCex<O> &svcomp)
  {
    if (dloc.isUnknown ()) return;
    std::string file;
    
    DIScope Scope (dloc.getScope ());
    if (Scope) file = Scope.getFilename ();
    else file = "<unknown>";
    
    svcomp.edge (file, (int)dloc.getLine (), "");
  }
  
  
  // static void printDecl (const CallInst &ci)
  // {
  //   const ConstantExpr *gep = 
  //     dyn_cast<const ConstantExpr> (ci.getArgOperand (0));
  //   errs () << "enter: " << constAsString (gep) << "\n";    
  // }
  
  static void printLineCex (std::vector<const BasicBlock*> const &cex)
  {
    if (SvCompCexFile.empty ()) return;
    
    std::error_code ec;
    llvm::tool_output_file out (SvCompCexFile.c_str (), ec, llvm::sys::fs::F_Text);
    if (ec)
    {
      errs () << "ERROR: Cannot open CEX file: " << ec.message () << "\n";
      return;
    }
    
    
    
    SvCompCex<llvm::raw_ostream> svcomp (out.os ());
    svcomp.header ();
    for (auto *bb : cex)
    {
      for (auto &I : *bb)
      {
        printDebugLoc (I.getDebugLoc (), svcomp);
        
        if (const CallInst *ci = dyn_cast<const CallInst> (&I))
        {
          Function *f = ci->getCalledFunction ();
          if (!f) continue;
          if (f->getName ().equals ("__UFO_liner"))
            printLiner (*ci, svcomp);
        }
      }
    }
    svcomp.footer ();
    out.keep ();
  }
  
  bool HornCex::runOnFunction (Function &F)
  {
    HornSolver &hs = getAnalysis<HornSolver> ();
    // -- only run if result is true, skip if it is false or unknown
    if (hs.getResult ()) ; else return false;
    
    LOG ("cex", 
         errs () << "Analyzed Function:\n"
         << F << "\n";);
    
    HornifyModule &hm = getAnalysis<HornifyModule> ();
    CutPointGraph &cpg = getAnalysis<CutPointGraph> (F);
    
    auto &fp = hs.getZFixedPoint ();
    ExprVector rules;
    fp.getCexRules (rules);
    boost::reverse (rules);
    
    // extract basic blocks
    std::vector<const BasicBlock*> bbTrace;
    std::vector<const CutPoint*> cpTrace;
    
    for (Expr r : rules)
    {
      Expr src, dst;
      if (isOpX<IMPL> (r)) 
      { 
        dst = r->arg (1);
        r = r->arg (0);
        src = isOpX<AND> (r) ? r->arg (0) : r;
      }
      else dst = r;
      if (src && !bind::isFapp (src)) src.reset (0);
      
      // -- if there is a src, then it was dst in previous iteration
      assert (bbTrace.empty () || bbTrace.back () == &hm.predicateBb (src));
      const BasicBlock *bb = &hm.predicateBb (dst);
      
      // XXX sometimes the cex includes the entry block, sometimes it does not
      // XXX normalize by removing it
      if (bb == &F.getEntryBlock ()) continue;
      
      bbTrace.push_back (bb);
      if (cpg.isCutPoint (*bb)) 
      {
        const CutPoint &cp = cpg.getCp2 (*bb);
        cpTrace.push_back (&cp);
      }
    }
    
    
    LOG ("cex", 
         errs () << "TRACE BEGIN\n";
         for (auto bb : bbTrace)
         {
           errs () << bb->getName ();
           if (cpg.isCutPoint (*bb)) errs () << " C";
           errs () << "\n";
         }
         errs () << "TRACE END\n";);
    
    
    ExprFactory &efac = hm.getExprFactory ();
    
    // -- local symbolic execution engine.
    // -- possibly different from the one used to solve the problem
    UfoSmallSymExec sem (efac, *this, MEM);
    // large step semantics to encode cp-to-cp edges
    UfoLargeSymExec lsem (sem);
    
    ExprVector side;
    std::vector<SymStore> states;
    std::vector<const CpEdge*> edges;
    
    states.push_back (SymStore (efac));
    const CutPoint *prev = &cpg.getCp2 (F.getEntryBlock ());
    for (const CutPoint *cp : cpTrace)
    {
      states.push_back (states.back ());
      SymStore &s = states.back ();
      
      // execute prev -> cp edge
      const CpEdge *edge = cpg.getEdge (*prev, *cp);
      assert (edge);
      edges.push_back (edge);
      lsem.execCpEdg (s, *edge, side);
      
      prev = cp;
    }
    
    
    ZSolver<EZ3> solver (hm.getZContext ());
    ExprVector assumptions;
    assumptions.reserve (side.size ());
    for (Expr v : side) 
    {
      Expr a = bind::boolConst (mk<ASM> (v));
      assumptions.push_back (a);
      solver.assertExpr (mk<IMPL> (a, v));
    }
    
    ExprVector core;
    solver.push ();
    auto res = solver.solveAssuming (assumptions);
    if (!res) solver.unsatCore (std::back_inserter (core));
    solver.pop ();
    
    LOG ("cex",
         errs () << "Solver: " 
         << (res ? "sat" : (!res ? "unsat" : "unknown")) << "\n";);
    
    LOG ("verbose_cex", 
         errs () << "CEX SIDE\n";
         for (auto a : side) errs () << *a << "\n";
         errs () << "CEX SIDE END\n";);
    
    if (res) ; else
    {
      // -- failed to validate the result
      errs () << "Initial core: " << core.size () << "\n";
      // poor-man's unsat core simplification
      while (core.size () < assumptions.size ())
      {
        assumptions.assign (core.begin (), core.end ());
        core.clear ();
        solver.push ();
        res = solver.solveAssuming (assumptions);
        assert (!res ? 1 : 0);
        solver.unsatCore (std::back_inserter (core));
        solver.pop ();
      }
      
      LOG("cex_core_min",
          // poor-man's unsat core minimization
          for (unsigned i = 0; i < core.size ();)
          {
            Expr saved = core [i];
            core [i] = core.back ();
            res = solver.solveAssuming 
              (boost::make_iterator_range (core.begin (), core.end () - 1));
            if (res) core [i++] = saved;
            else if (!res) 
            {
              errs () << "core removed: " << *saved << "\n";
              core.pop_back ();
              
            }
            
            else assert (0);
          });
      
      
      
      errs () << "Final core: " << core.size () << "\n";
      
      errs () << "Failed to validate CEX. Core is: \n";
      ExprVector coreE (core.begin (), core.end ());
      for (Expr &c : coreE) c = bind::fname (bind::fname (c))->arg (0);
      
      ExprMap map;
      // simplify (efac, coreE, map);
      // for (Expr &c : coreE) c = z3_simplify (hm.getZContext (), c);
      
      for (Expr c : coreE) errs () << *c << "\n";
      
      Stats::sset("Result", "FAILED");
      return false;
    }
    
    auto mdl (solver.getModel ());
    ExprVector trace;
    trace.reserve (side.size ());
    
    // compute implicant
    for (auto v : side)
    {
      // -- break IMPL into an OR
      // -- OR into a single disjunct
      // -- single disjunct into an AND
      if (isOpX<IMPL> (v))
      {
        Expr a0 = mdl (v->arg (0));
        if (isOpX<FALSE> (a0)) continue;
        else if (isOpX<TRUE> (a0))
          v = mknary<OR> (mk<FALSE> (efac), 
                          ++(v->args_begin ()), v->args_end ());
        else
          continue;
      }
      
      if (isOpX<OR> (v))
      {
        for (unsigned i = 0; i < v->arity (); ++i)
          if (isOpX<TRUE> (mdl (v->arg (i))))
          {
            v = v->arg (i);
            break;
          }
      }
        
      if (isOpX<AND> (v)) 
      {
        for (unsigned i = 0; i < v->arity (); ++i)
          trace.push_back (v->arg (i));
      }
      else trace.push_back (v);
    }
    
    
    // lookup table for the implicant
    boost::sort (trace);
    boost::container::flat_set<Expr> implicant (trace.begin (), trace.end ());
    
    std::vector<const BasicBlock*> cex;
    
    // -- walk edges and symbolic states and extract the trace and values
    auto st = states.begin ();
    for (const CpEdge *edge : edges)
    {
      SymStore &s = *(++st);
      
      for (auto it = edge->begin (), end = edge->end (); it != end; ++it)
      {
        const BasicBlock &BB = *it;
        
        // -- not on implicant == not in the trace
        if (it != edge->begin () && 
            implicant.count (s.eval (sem.symb (BB))) <= 0) continue;
        
        // -- print the counterexample is debug format
        LOG ("cex",
             errs () << BB.getName () << ": \n";
        
             for (auto &I : BB)
             {
               if (!isa<PHINode> (I) && sem.isTracked (I))
                 errs () << "  %" << I.getName () << " " 
                         << *mdl.eval (s.eval (sem.symb (I)), true);
               errs () << "\n";
             });
        cex.push_back (&BB);
      }
    }
    
    if (edges.empty ())
      // -- special case when the problem is trivial 
      // -- the entry block contains the error location
      cex.push_back (&F.getEntryBlock ());
    else
      // -- last bb of the last edge
      cex.push_back (&edges.back ()->target ().bb ());
    
    printLineCex (cex);
    
    // at this point, vector cex contains the counterexample is the
    //proper order. Can construct the necessary XML out of this.

    
    return false;
  }
  
  void HornCex::getAnalysisUsage (AnalysisUsage &AU) const
  {
    AU.setPreservesAll ();
    AU.addRequired<DataLayoutPass> ();
    AU.addRequired<CutPointGraph> ();
    AU.addRequired<HornifyModule> ();
    AU.addRequired<HornSolver> ();
    AU.addRequired<CanFail> ();
  }
}

