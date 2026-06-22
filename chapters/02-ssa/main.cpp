#include "ssa.h"

#include <cassert>
#include <iostream>
#include <sstream>

using namespace bc;

// Build this by hand, with `m` reassigned on two paths:
//
//   fn classify(a, b) {
//   entry:
//     c   = a + b
//     cmp = a < b
//     if cmp -> then else -> els
//   then: m = b ; -> done
//   els:  m = a ; -> done
//   done:
//     r = m + c        // m here is the merge of the two assignments
//     ret r
//   }
//
// `m` is live out of both `then` and `els` and read in `done`, so SSA needs a
// phi at `done`. `c` is defined once and dominates its use, so it gets no phi.
int main() {
  Function fn;
  fn.name = "classify";
  fn.retType = Type::I64;
  fn.addArg("a", Type::I64);
  fn.addArg("b", Type::I64);

  Variable *c = fn.addVar("c", Type::I64);
  Variable *cmp = fn.addVar("cmp", Type::I1);
  Variable *m = fn.addVar("m", Type::I64);
  Variable *r = fn.addVar("r", Type::I64);

  BasicBlock *entry = fn.addBlock("entry");
  BasicBlock *then = fn.addBlock("then");
  BasicBlock *els = fn.addBlock("els");
  BasicBlock *done = fn.addBlock("done");

  entry->addAssign(c, Op::Add, {arg("a"), arg("b")});
  entry->addAssign(cmp, Op::ICmpLt, {arg("a"), arg("b")});
  entry->setCondBr(rd(cmp), then, els);

  then->addAssign(m, Op::Copy, {arg("b")});
  then->setBr(done);

  els->addAssign(m, Op::Copy, {arg("a")});
  els->setBr(done);

  done->addAssign(r, Op::Add, {rd(m), rd(c)});
  done->setRet(rd(r));

  std::cout << "before SSA:\n";
  printPre(std::cout, fn);

  toSSA(fn);

  std::cout << "\nafter SSA:\n";
  printSSA(std::cout, fn);

  // dominators: every block is dominated by entry; `done` joins then/els.
  assert(entry->idom == entry);
  assert(then->idom == entry);
  assert(done->idom == entry);

  // dominance frontier: then and els both stop dominating at the join.
  assert(then->df.count(done) == 1);
  assert(els->df.count(done) == 1);
  assert(entry->df.empty());

  // exactly one phi, at done, for m, with one input per predecessor.
  assert(done->phis.size() == 1);
  assert(done->phis[0].var == m);
  assert(done->phis[0].incoming.size() == 2);
  assert(then->phis.empty() && els->phis.empty());

  // c and r were defined once and never need a phi.
  std::ostringstream os;
  printSSA(os, fn);
  std::string text = os.str();
  assert(text.find("phi i64") != std::string::npos);
  assert(text.find("%m.2 = phi") != std::string::npos);
  assert(text.find("phi i1") == std::string::npos);  // cmp got no phi

  std::cout << "\nchecks passed\n";
  return 0;
}
