#include "optimize.h"

#include <cassert>
#include <iostream>

using namespace bc;

// A function picked so all three passes have something to chew on:
//
//   fn compute(p) {
//   entry:
//     a = copy 6
//     b = copy 7
//     c = mul a, b          // foldable: a and b are constants -> 42
//     br check
//   check:
//     cond = icmp lt p, c   // p is an arg (unknown), so cond stays a real value
//     condbr cond, hot, cold
//   hot:
//     r = add c, 8          // 42 + 8 -> 50
//     br done
//   cold:
//     r = sub c, 2          // 42 - 2 -> 40
//     br done
//   done:
//     unused = mul c, c     // 1764, but nobody reads it -> dead
//     ret r                 // r is 50 on one path, 40 on the other -> bottom
//   }
//
// Constant propagation pins a, b, c (and unused) to constants but leaves r at
// bottom, because the two predecessors of `done` disagree about it. Folding
// turns the foldable instructions into copies and rewrites the `c` operand
// inside `cond` into the immediate 42. Once c's readers are all immediates, a,
// b, and c have no users left, so DCE sweeps them out along with `unused`.
int main() {
  Function fn;
  fn.name = "compute";
  fn.retType = Type::I64;
  fn.addArg("p", Type::I64);

  Variable *a = fn.addVar("a", Type::I64);
  Variable *b = fn.addVar("b", Type::I64);
  Variable *c = fn.addVar("c", Type::I64);
  Variable *cond = fn.addVar("cond", Type::I1);
  Variable *r = fn.addVar("r", Type::I64);
  Variable *unused = fn.addVar("unused", Type::I64);

  BasicBlock *entry = fn.addBlock("entry");
  BasicBlock *check = fn.addBlock("check");
  BasicBlock *hot = fn.addBlock("hot");
  BasicBlock *cold = fn.addBlock("cold");
  BasicBlock *done = fn.addBlock("done");

  entry->addAssign(a, Op::Copy, {cst(6)});
  entry->addAssign(b, Op::Copy, {cst(7)});
  entry->addAssign(c, Op::Mul, {rd(a), rd(b)});
  entry->setBr(check);

  check->addAssign(cond, Op::ICmpLt, {arg("p"), rd(c)});
  check->setCondBr(rd(cond), hot, cold);

  hot->addAssign(r, Op::Add, {rd(c), cst(8)});
  hot->setBr(done);

  cold->addAssign(r, Op::Sub, {rd(c), cst(2)});
  cold->setBr(done);

  done->addAssign(unused, Op::Mul, {rd(c), rd(c)});
  done->setRet(rd(r));

  std::cout << "before:\n";
  printFunction(std::cout, fn);

  // Run the analysis on its own first so we can see (and check) the lattice.
  DataflowResult<ConstMap> cp = runConstProp(fn);
  std::cout << "\n";
  printConstProp(std::cout, fn, cp);

  // c is the same constant everywhere downstream; r meets to bottom at `done`.
  assert(cvGet(cp.out.at(entry), c) == cvConst(42));
  assert(cvGet(cp.in.at(done), r) == cvBot());
  assert(cvGet(cp.out.at(done), unused) == cvConst(1764));

  // Now the rewrite pipeline.
  foldConstants(fn);
  deadCodeElim(fn);

  std::cout << "\nafter:\n";
  printFunction(std::cout, fn);

  // entry's a, b, c all became dead once their readers folded to immediates.
  assert(entry->code.empty());

  // The condition survives (it reads the argument), but its `c` operand is now
  // the literal 42.
  assert(check->code.size() == 1);
  assert(check->code[0].op == Op::ICmpLt);
  assert(check->code[0].uses[1].kind == Use::Kind::Const);
  assert(check->code[0].uses[1].imm == 42);

  // The two arms folded to plain copies of their results.
  assert(hot->code.size() == 1);
  assert(hot->code[0].op == Op::Copy);
  assert(hot->code[0].uses[0].imm == 50);

  assert(cold->code.size() == 1);
  assert(cold->code[0].op == Op::Copy);
  assert(cold->code[0].uses[0].imm == 40);

  // `unused` was a fine constant and still pointless, so it's gone.
  assert(done->code.empty());

  std::cout << "\nchecks passed\n";
  return 0;
}
