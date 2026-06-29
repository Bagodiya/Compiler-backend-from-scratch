#include "regalloc.h"

#include <cassert>
#include <iostream>
#include <set>
#include <string>

using namespace bc;

// A single straight-line block, chosen so the allocator has to make every kind
// of decision: hand out registers, reclaim them as values die, and spill when
// it runs out.
//
//   fn ra(i64 a, i64 b) -> i64 {
//   entry:
//     base = add a, b      // computed early, not read again until the very end
//     m0   = mul a, a
//     m1   = mul b, b
//     m2   = mul a, b
//     s0   = add m0, m1
//     s1   = add s0, m2
//     r    = add s1, base  // base finally used here
//     ret r
//   }
//
// Around the middle, base / a / b / m0 / m1 are all live at once and m2 wants a
// register too -- six values, and we only give the allocator five. `base` lives
// longest, so it loses the contest and gets spilled to the stack. Everything
// else fits, and registers freed by dead values (a, b, ...) get reused.

static const std::vector<std::string> kPool = {"rbx", "r10", "r11", "r12",
                                               "r13"};

int main() {
  Function fn;
  fn.name = "ra";
  fn.retType = Type::I64;
  fn.addArg("a", Type::I64);
  fn.addArg("b", Type::I64);

  Variable *base = fn.addVar("base", Type::I64);
  Variable *m0 = fn.addVar("m0", Type::I64);
  Variable *m1 = fn.addVar("m1", Type::I64);
  Variable *m2 = fn.addVar("m2", Type::I64);
  Variable *s0 = fn.addVar("s0", Type::I64);
  Variable *s1 = fn.addVar("s1", Type::I64);
  Variable *r = fn.addVar("r", Type::I64);

  BasicBlock *entry = fn.addBlock("entry");
  entry->addAssign(base, Op::Add, {arg("a"), arg("b")});
  entry->addAssign(m0, Op::Mul, {arg("a"), arg("a")});
  entry->addAssign(m1, Op::Mul, {arg("b"), arg("b")});
  entry->addAssign(m2, Op::Mul, {arg("a"), arg("b")});
  entry->addAssign(s0, Op::Add, {rd(m0), rd(m1)});
  entry->addAssign(s1, Op::Add, {rd(s0), rd(m2)});
  entry->addAssign(r, Op::Add, {rd(s1), rd(base)});
  entry->setRet(rd(r));

  std::cout << "IR:\n";
  printFunction(std::cout, fn);

  MFunction mf = selectFunction(fn);
  std::cout << "\nselected (virtual registers):\n";
  printMFunction(std::cout, mf);

  AllocResult R = linearScan(mf, kPool);
  std::cout << "\n";
  printIntervals(std::cout, R);
  std::cout << "\n";
  printAllocation(std::cout, R);

  applyAllocation(mf, R);
  std::cout << "\nafter allocation (" << kPool.size()
            << " registers, spills to the stack):\n";
  printMFunction(std::cout, mf);

  // Look up an interval / allocation by name, since vreg ids are internal.
  auto ivOf = [&](const std::string &n) -> Interval {
    for (const Interval &i : R.intervals)
      if (R.names.at(i.vreg) == n) return i;
    assert(false && "no such interval");
    return Interval{};
  };

  // base spans the whole function; a dies less than halfway through. That gap is
  // exactly why a's register can be recycled and base's can't.
  assert(ivOf("base").start == 0 && ivOf("base").end == 13);
  assert(ivOf("a").start == 0 && ivOf("a").end == 6);

  // Exactly one value gets spilled, and it's the longest-lived one: base.
  int spills = 0;
  std::string spilledName;
  for (auto &kv : R.alloc)
    if (kv.second.spilled) {
      ++spills;
      spilledName = R.names.at(kv.first);
    }
  assert(spills == 1);
  assert(R.numSpills == 1);
  assert(spilledName == "base");

  // The other eight values share just five physical registers, so reuse had to
  // happen (eight live ranges, five names).
  std::set<std::string> regsUsed;
  int inReg = 0;
  for (auto &kv : R.alloc)
    if (!kv.second.spilled) {
      regsUsed.insert(kv.second.reg);
      ++inReg;
    }
  assert(inReg == 8);
  assert(regsUsed.size() == kPool.size());  // all five, fully used

  // After the rewrite, base reads/writes the stack. Its definition (mov at the
  // top of entry) writes a memory slot, and the final add reads it back.
  MBlock &e = mf.blocks[0];
  assert(e.insts.front().op == MOp::Mov);
  assert(e.insts.front().ops[0].kind == MOperand::Kind::Mem);  // mov [rbp-8], ...
  const MInst &lastAdd = e.insts[13];
  assert(lastAdd.op == MOp::Add);
  assert(lastAdd.ops[1].kind == MOperand::Kind::Mem);  // add %r, [rbp-8]

  std::cout << "\nchecks passed\n";
  return 0;
}
