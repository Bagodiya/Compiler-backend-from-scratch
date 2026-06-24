#include "dataflow.h"

#include <cassert>
#include <iostream>

using namespace bc;

// A loop, because a loop is what forces the iteration to actually iterate. A
// straight-line function would converge in a single backward pass; the back
// edge means live-out of `body` depends on live-in of `loop`, which depends
// right back on `body`, so the solver has to go around twice.
//
//   fn count(n) {
//   entry:
//     i = 0
//     s = 0
//     br loop
//   loop:
//     cmp = i < n          // n is an argument, not tracked
//     condbr cmp, body, exit
//   body:
//     s = s + i
//     i = i + 1
//     br loop              // <- back edge
//   exit:
//     ret s
//   }
//
// i and s are live all the way around the loop. cmp is born and dies inside
// `loop`, so it never shows up in any live set: that's liveness pruning a
// short-lived temporary, which is exactly the signal register allocation wants.
int main() {
  Function fn;
  fn.name = "count";
  fn.retType = Type::I64;
  fn.addArg("n", Type::I64);

  Variable *i = fn.addVar("i", Type::I64);
  Variable *s = fn.addVar("s", Type::I64);
  Variable *cmp = fn.addVar("cmp", Type::I1);

  BasicBlock *entry = fn.addBlock("entry");
  BasicBlock *loop = fn.addBlock("loop");
  BasicBlock *body = fn.addBlock("body");
  BasicBlock *exit = fn.addBlock("exit");

  entry->addAssign(i, Op::Copy, {cst(0)});
  entry->addAssign(s, Op::Copy, {cst(0)});
  entry->setBr(loop);

  loop->addAssign(cmp, Op::ICmpLt, {rd(i), arg("n")});
  loop->setCondBr(rd(cmp), body, exit);

  body->addAssign(s, Op::Add, {rd(s), rd(i)});
  body->addAssign(i, Op::Add, {rd(i), cst(1)});
  body->setBr(loop);

  exit->setRet(rd(s));

  std::cout << "the function:\n";
  printFunction(std::cout, fn);

  DataflowResult<VarSet> live = runLiveness(fn);

  std::cout << "\n";
  printLiveness(std::cout, fn, live);

  // What we expect, worked out by hand:
  //   exit:  in {s},   out {}
  //   body:  in {i,s}, out {i,s}
  //   loop:  in {i,s}, out {i,s}
  //   entry: in {},    out {i,s}
  VarSet is = {i, s};

  assert(live.in.at(exit) == VarSet({s}));
  assert(live.out.at(exit) == VarSet());

  assert(live.in.at(body) == is);
  assert(live.out.at(body) == is);

  assert(live.in.at(loop) == is);
  assert(live.out.at(loop) == is);

  // The arguments are live into entry, but nothing the body wrote is, so
  // live-in of entry is empty and live-out carries the loop's needs back up.
  assert(live.in.at(entry) == VarSet());
  assert(live.out.at(entry) == is);

  // cmp is defined and consumed inside `loop` and never crosses a block edge,
  // so it appears in no live-in or live-out set anywhere.
  for (auto &bp : fn.blocks) {
    assert(live.in.at(bp.get()).count(cmp) == 0);
    assert(live.out.at(bp.get()).count(cmp) == 0);
  }

  std::cout << "checks passed\n";
  return 0;
}
