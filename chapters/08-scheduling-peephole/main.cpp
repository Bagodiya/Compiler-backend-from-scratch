#include "schedule.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using namespace bc;

// One straight-line block built so scheduling has an obvious, and legal, move.
//
//   fn sched(i64 %a, i64 %b, i64 %c) -> i64 {
//   entry:
//     x = c + c        // a cheap add chain, emitted first...
//     y = x + 1
//     p = a * b        // ...and a slow, independent multiply, emitted last
//     q = p * p
//     r = y + q        // the two chains meet
//     s = r + c        // and c is used again here, so it stays live throughout
//     ret s
//   }
//
// The multiply chain is the critical path -- two imuls at latency 3 each -- but
// the selector emits it after the cheap adds. Run in program order, a machine
// would grind through the adds first and only then start the slow multiplies,
// stalling at the tail. The scheduler sees the multiply chain has the longest
// critical path and hoists it up front, so the multiplies are in flight while the
// adds run.
//
// The hoist is legal only because after allocation the multiply lands in
// registers the add chain never reads: a and b die into it, and I keep c alive to
// the end (the `s = r + c`) so its register can't be handed to p. If p had reused
// c's register, moving it above the adds would clobber c before they read it --
// an anti-dependence the scheduler would see and refuse to cross. That is exactly
// why this pass runs on physical registers, after allocation.
//
// The two-address lowering then leaves self-moves behind (a copy whose ends were
// colored the same register). Peephole sweeps those away.

static const std::vector<std::string> kPool = {"rbx", "r12", "r13", "r14"};

static int countSelfMoves(const MFunction &mf) {
  int n = 0;
  for (const MBlock &b : mf.blocks)
    for (const MInst &in : b.insts)
      if (in.op == MOp::Mov && in.ops[0].kind == MOperand::Kind::Reg &&
          in.ops[1].kind == MOperand::Kind::Reg &&
          in.ops[0].name == in.ops[1].name)
        ++n;
  return n;
}

// A tiny interpreter for a single straight-line block of allocated code. It reads
// the Ret operand's value given starting values in the argument registers. This
// is the real correctness check: reordering and peephole must not change it.
static int64_t simulate(const MFunction &mf,
                        const std::map<std::string, int64_t> &argRegs) {
  std::map<std::string, int64_t> R = argRegs;
  auto val = [&](const MOperand &o) -> int64_t {
    if (o.kind == MOperand::Kind::Reg) return R[o.name];
    if (o.kind == MOperand::Kind::Imm) return o.imm;
    return 0;
  };
  for (const MBlock &b : mf.blocks)
    for (const MInst &in : b.insts) {
      switch (in.op) {
      case MOp::Mov:  R[in.ops[0].name] = val(in.ops[1]); break;
      case MOp::Add:  R[in.ops[0].name] = val(in.ops[0]) + val(in.ops[1]); break;
      case MOp::Sub:  R[in.ops[0].name] = val(in.ops[0]) - val(in.ops[1]); break;
      case MOp::IMul: R[in.ops[0].name] = val(in.ops[0]) * val(in.ops[1]); break;
      case MOp::Ret:  return val(in.ops[0]);
      default: break;  // no branches in this example
      }
    }
  return 0;
}

int main() {
  Function fn;
  fn.name = "sched";
  fn.retType = Type::I64;
  fn.addArg("a", Type::I64);
  fn.addArg("b", Type::I64);
  fn.addArg("c", Type::I64);

  Variable *x = fn.addVar("x", Type::I64);
  Variable *y = fn.addVar("y", Type::I64);
  Variable *p = fn.addVar("p", Type::I64);
  Variable *q = fn.addVar("q", Type::I64);
  Variable *r = fn.addVar("r", Type::I64);
  Variable *s = fn.addVar("s", Type::I64);

  BasicBlock *entry = fn.addBlock("entry");
  entry->addAssign(x, Op::Add, {arg("c"), arg("c")});
  entry->addAssign(y, Op::Add, {rd(x), cst(1)});
  entry->addAssign(p, Op::Mul, {arg("a"), arg("b")});
  entry->addAssign(q, Op::Mul, {rd(p), rd(p)});
  entry->addAssign(r, Op::Add, {rd(y), rd(q)});
  entry->addAssign(s, Op::Add, {rd(r), arg("c")});
  entry->setRet(rd(s));

  std::cout << "IR:\n";
  printFunction(std::cout, fn);

  // Select, then allocate registers with the chapter-7 machinery. Scheduling is a
  // post-pass over the allocated code, so it runs after this.
  MFunction mf = selectFunction(fn);
  Values V = collectValues(fn);
  std::vector<int> uses = countUses(fn, V);
  Liveness live = computeLiveness(fn, V);
  Graph G = buildInterference(fn, V, live);
  Coloring C = colorGraph(G, static_cast<int>(kPool.size()), uses);
  applyColoring(mf, V, C, kPool);
  assert(C.spilled.empty() && "example is meant to fit without spilling");

  std::cout << "\nallocated (physical registers, program order):\n";
  printMFunction(std::cout, mf);

  // The argument registers, and a reference result to check the code against.
  // With a=3, b=4, c=5: x=10, y=11, p=12, q=144, r=155, s=160.
  std::map<std::string, int64_t> argRegs;
  argRegs[kPool[C.color[V.argId.at("a")]]] = 3;
  argRegs[kPool[C.color[V.argId.at("b")]]] = 4;
  argRegs[kPool[C.color[V.argId.at("c")]]] = 5;
  const int64_t expected = 160;
  assert(simulate(mf, argRegs) == expected && "allocation itself is wrong");

  // Plan the schedule for the entry block so we can inspect it before applying.
  BlockSchedule plan = planSchedule(mf.blocks[0]);
  std::cout << "\n";
  printCriticalPaths(std::cout, plan.body, plan.dag);

  scheduleFunction(mf);
  std::cout << "\nafter scheduling:\n";
  printMFunction(std::cout, mf);
  assert(simulate(mf, argRegs) == expected && "scheduling changed the result");

  int selfBefore = countSelfMoves(mf);
  int rewritten = peepholeFunction(mf);
  std::cout << "\nafter peephole (removed/rewrote " << rewritten << "):\n";
  printMFunction(std::cout, mf);
  assert(simulate(mf, argRegs) == expected && "peephole changed the result");

  // --- checks --------------------------------------------------------------

  // The schedule is a permutation of the body: every index appears exactly once.
  {
    std::vector<bool> seen(plan.dag.n, false);
    assert(static_cast<int>(plan.order.size()) == plan.dag.n);
    for (int idx : plan.order) {
      assert(!seen[idx]);
      seen[idx] = true;
    }
  }

  // It respects every dependence edge: a producer lands before anything that
  // depends on it. On physical registers this covers the anti- and output-
  // dependences from register reuse too, which is what makes the reorder legal.
  std::vector<int> pos(plan.dag.n);
  for (int k = 0; k < plan.dag.n; ++k) pos[plan.order[k]] = k;
  for (int i = 0; i < plan.dag.n; ++i)
    for (int j : plan.dag.succ[i])
      assert(pos[i] < pos[j] && "schedule broke a dependence edge");

  // The scheduler leads with the longest critical path.
  {
    int first = plan.order[0];
    for (int v = 0; v < plan.dag.n; ++v)
      assert(plan.dag.cp[first] >= plan.dag.cp[v]);
  }

  // The multiply really moved: the first imul in the original body got pulled
  // ahead of the add chain that preceded it.
  {
    int origFirstMul = -1;
    for (int i = 0; i < plan.dag.n; ++i)
      if (plan.body[i].op == MOp::IMul) {
        origFirstMul = i;
        break;
      }
    assert(origFirstMul >= 0);
    assert(pos[origFirstMul] < origFirstMul && "imul was not hoisted");
  }

  // Peephole cleared out the self-moves: there were some, and now there are none.
  assert(selfBefore > 0);
  assert(countSelfMoves(mf) == 0);
  assert(rewritten >= selfBefore);

  // A direct check on every peephole rule, on a hand-built block, so nothing ships
  // untested just because this example didn't happen to trigger it.
  {
    MBlock t;
    t.label = "t";
    emit(t, MOp::Mov, {mreg(0, "rbx"), mreg(0, "rbx")});   // self-move, drop
    emit(t, MOp::Add, {mreg(0, "rbx"), mimm(0)});          // add r,0, drop
    emit(t, MOp::IMul, {mreg(0, "rbx"), mimm(1)});         // imul r,1, drop
    emit(t, MOp::IMul, {mreg(0, "rbx"), mimm(2)});         // imul r,2 -> add r,r
    emit(t, MOp::Mov, {mreg(0, "rbx"), mreg(1, "r12")});   // real move, keep
    int n = peepholeBlock(t);
    assert(n == 4);
    assert(t.insts.size() == 2);
    assert(t.insts[0].op == MOp::Add);
    assert(t.insts[0].ops[0].name == "rbx" && t.insts[0].ops[1].name == "rbx");
    assert(t.insts[1].op == MOp::Mov && t.insts[1].ops[1].name == "r12");
  }

  std::cout << "\nchecks passed\n";
  return 0;
}
