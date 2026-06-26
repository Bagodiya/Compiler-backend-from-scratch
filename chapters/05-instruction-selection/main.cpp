#include "isel.h"

#include <cassert>
#include <iostream>

using namespace bc;

// The same shape as chapter 1's example, picked so selection has each of its
// tricks to show:
//
//   fn sel(i64 a, i64 b) -> i64 {
//   entry:
//     t    = add a, b          // two-address: mov then add
//     u    = add t, 10         // immediate 10 folds into the add
//     cond = icmp lt u, 100    // compare feeding straight into the branch...
//     condbr cond, small, big  // ...so it fuses: cmp + jl, no setl
//   small:
//     ret u
//   big:
//     s = sub u, 100           // immediate 100 folds into the sub
//     ret s
//   }
static MBlock *blockNamed(MFunction &mf, const std::string &label) {
  for (MBlock &b : mf.blocks)
    if (b.label == label) return &b;
  return nullptr;
}

int main() {
  Function fn;
  fn.name = "sel";
  fn.retType = Type::I64;
  fn.addArg("a", Type::I64);
  fn.addArg("b", Type::I64);

  Variable *t = fn.addVar("t", Type::I64);
  Variable *u = fn.addVar("u", Type::I64);
  Variable *cond = fn.addVar("cond", Type::I1);
  Variable *s = fn.addVar("s", Type::I64);

  BasicBlock *entry = fn.addBlock("entry");
  BasicBlock *small = fn.addBlock("small");
  BasicBlock *big = fn.addBlock("big");

  entry->addAssign(t, Op::Add, {arg("a"), arg("b")});
  entry->addAssign(u, Op::Add, {rd(t), cst(10)});
  entry->addAssign(cond, Op::ICmpLt, {rd(u), cst(100)});
  entry->setCondBr(rd(cond), small, big);

  small->setRet(rd(u));

  big->addAssign(s, Op::Sub, {rd(u), cst(100)});
  big->setRet(rd(s));

  std::cout << "IR:\n";
  printFunction(std::cout, fn);

  MFunction mf = selectFunction(fn);
  std::cout << "\nselected (virtual registers):\n";
  printMFunction(std::cout, mf);

  // entry should be:
  //   mov %t, %a
  //   add %t, %b
  //   mov %u, %t
  //   add %u, 10
  //   cmp %u, 100
  //   jl small
  //   jmp big
  MBlock *e = blockNamed(mf, "entry");
  assert(e && e->insts.size() == 7);

  // Two-address expansion of `t = add a, b`.
  assert(e->insts[0].op == MOp::Mov);
  assert(e->insts[1].op == MOp::Add);

  // The immediate 10 folded into the add rather than being moved first.
  assert(e->insts[3].op == MOp::Add);
  assert(e->insts[3].ops[1].kind == MOperand::Kind::Imm);
  assert(e->insts[3].ops[1].imm == 10);

  // The compare fused into the branch: cmp, then a conditional jump, then the
  // fallthrough jump. No setl anywhere -- the boolean was never materialized.
  assert(e->insts[4].op == MOp::Cmp);
  assert(e->insts[4].ops[1].imm == 100);
  assert(e->insts[5].op == MOp::Jl);
  assert(e->insts[5].ops[0].name == "small");
  assert(e->insts[6].op == MOp::Jmp);
  assert(e->insts[6].ops[0].name == "big");

  bool anySetl = false;
  for (MBlock &b : mf.blocks)
    for (MInst &i : b.insts)
      if (i.op == MOp::Setl) anySetl = true;
  assert(!anySetl);

  // big: mov %s, %u ; sub %s, 100 ; ret %s
  MBlock *bg = blockNamed(mf, "big");
  assert(bg && bg->insts.size() == 3);
  assert(bg->insts[0].op == MOp::Mov);
  assert(bg->insts[1].op == MOp::Sub);
  assert(bg->insts[1].ops[1].imm == 100);
  assert(bg->insts[2].op == MOp::Ret);

  std::cout << "\nchecks passed\n";
  return 0;
}
