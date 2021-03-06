#include "seahorn/HornWrite.hh"
#include "seahorn/HornifyModule.hh"
#include "seahorn/HornClauseDBTransf.hh"
#include "seahorn/ClpWrite.hh"

#include "llvm/Support/CommandLine.h"

static llvm::cl::opt<bool>
InternalWriter("horn-fp-internal-writer",
               llvm::cl::desc("Use internal writer for Horn SMT2 format. (Default)"),
               llvm::cl::init(true),llvm::cl::Hidden);

enum HCFormat { SMT2, CLP, PURESMT2};
static llvm::cl::opt<HCFormat>
HornClauseFormat("horn-format",
       llvm::cl::desc ("Specify the format for Horn Clauses"),
       llvm::cl::values 
       (clEnumValN (SMT2,"smt2",
                    "SMT2 (default)"),
        clEnumValN (CLP, "clp",
                    "CLP (Constraint Logic Programming)"),
        clEnumValN (PURESMT2, "pure-smt2",
                    "Pure SMT-LIB2 compliant format"),
        clEnumValEnd),
       llvm::cl::init (SMT2));

namespace seahorn
{
  char HornWrite::ID = 0;
  
  void HornWrite::getAnalysisUsage (AnalysisUsage &AU) const
  {
    AU.addRequired<HornifyModule> ();
    AU.setPreservesAll ();
  }
  
  bool HornWrite::runOnModule (Module &M)
  {
    HornifyModule &hm = getAnalysis<HornifyModule> ();
    HornClauseDB &db  = hm.getHornClauseDB ();
    ExprFactory &efac = hm.getExprFactory ();

    if (HornClauseFormat == CLP)
    {
      normalizeHornClauseHeads (db);
      ClpWrite writer (db, efac);
      m_out << writer.toString ();
    }
    else 
    {
      // Use local ZFixedPoint object to translate to SMT2. 
      //
      // When HornWrite is called hm.getZFixedPoint () might be still
      // empty so we need to dump first the content of HornClauseDB
      // into fp.
      ZFixedPoint<EZ3> fp (hm.getZContext ());
      // -- skip constraints since they are not supported.
      // -- do not skip the query
      db.loadZFixedPoint (fp, true, false);

      if (HornClauseFormat == PURESMT2)
      {
        // -- disable fixedpoint extension
        ZParams<EZ3> params (hm.getZContext ());
        params.set (":print_fixedpoint_extensions", false);
        fp.set (params);
      }
      
      if (HornClauseFormat == PURESMT2 || !InternalWriter)
        m_out << fp.toString () << "\n";
      else
        m_out << fp << "\n";
    }
    
    m_out.flush ();
    return false;
  }
  
}
