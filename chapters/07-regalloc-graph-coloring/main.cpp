#include "regalloc.h"

#include <cassert>
#include <iostream>
#include <set>
#include <string>

using namespace bc;

// A single straight-line block, built so the interference graph has one tight
// spot. Four values are live across the same instruction, and I only hand the
// allocator three registers, so exactly one of the four has to go to memory.
//
//   fn gc() -> i64 {
//   entry:
//     L = copy 7        // L is read again and again, all the way to the end
//     s = copy 8        // s is read exactly once, at w
//     x = copy 9
//     y = copy 10       // here L, s, x, y are all live at once -- four, K is 3
//     z = x + y         // x, y die
//     w = z + s         // s dies
//     v = w + L
//     u = v + L
//     r = u + L         // L finally dies
//     ret r
//   }
//
// The crunch is {L, s, x, y}: a four-clique in the interference graph. Linear
// scan, sweeping left to right, would spill the interval that reaches furthest --
// that's L, the one read four times. Graph coloring weighs the whole graph and
// spills the cheapest member of the clique instead: s, read just once. Same
// pressure relieved, a quarter of the reload traffic.

static const std::vector<std::string> kPool = {"rbx", "r12", "r13"};  // K = 3

int main() {
  Function fn;
  fn.name = "gc";
  fn.retType = Type::I64;

  Variable *L = fn.addVar("L", Type::I64);
  Variable *s = fn.addVar("s", Type::I64);
  Variable *x = fn.addVar("x", Type::I64);
  Variable *y = fn.addVar("y", Type::I64);
  Variable *z = fn.addVar("z", Type::I64);
  Variable *w = fn.addVar("w", Type::I64);
  Variable *v = fn.addVar("v", Type::I64);
  Variable *u = fn.addVar("u", Type::I64);
  Variable *r = fn.addVar("r", Type::I64);

  BasicBlock *entry = fn.addBlock("entry");
  entry->addAssign(L, Op::Copy, {cst(7)});
  entry->addAssign(s, Op::Copy, {cst(8)});
  entry->addAssign(x, Op::Copy, {cst(9)});
  entry->addAssign(y, Op::Copy, {cst(10)});
  entry->addAssign(z, Op::Add, {rd(x), rd(y)});
  entry->addAssign(w, Op::Add, {rd(z), rd(s)});
  entry->addAssign(v, Op::Add, {rd(w), rd(L)});
  entry->addAssign(u, Op::Add, {rd(v), rd(L)});
  entry->addAssign(r, Op::Add, {rd(u), rd(L)});
  entry->setRet(rd(r));

  std::cout << "IR:\n";
  printFunction(std::cout, fn);

  Values V = collectValues(fn);
  std::vector<int> uses = countUses(fn, V);
  Liveness live = computeLiveness(fn, V);
  Graph G = buildInterference(fn, V, live);
  std::cout << "\n";
  printInterference(std::cout, V, G);

  Coloring C = colorGraph(G, static_cast<int>(kPool.size()), uses);
  std::cout << "\n";
  printColoring(std::cout, V, C, kPool);

  MFunction mf = selectFunction(fn);
  std::cout << "\nselected (virtual registers):\n";
  printMFunction(std::cout, mf);

  applyColoring(mf, V, C, kPool);
  std::cout << "\nafter allocation (3 registers, spill to the stack):\n";
  printMFunction(std::cout, mf);

  // Look things up by name, since value ids are internal.
  auto idOf = [&](const std::string &n) -> int {
    for (int i = 0; i < V.size(); ++i)
      if (V.name[i] == n) return i;
    assert(false && "no such value");
    return -1;
  };
  auto isSpilled = [&](const std::string &n) {
    return C.spilled.count(idOf(n)) != 0;
  };

  // The four-clique really is mutual: every pair among L, s, x, y interferes.
  const std::vector<std::string> clique = {"L", "s", "x", "y"};
  for (const std::string &a : clique)
    for (const std::string &b : clique)
      if (a != b)
        assert(G.adj[idOf(a)].count(idOf(b)) && "clique edge missing");

  // Exactly one value spills, and it's s -- the cheap member of the clique.
  assert(C.spilled.size() == 1);
  assert(isSpilled("s"));

  // The heavily-used long value L kept a register; that's the whole point.
  assert(!isSpilled("L"));
  assert(C.color[idOf("L")] >= 0);

  // s really is cheaper to spill than L would have been.
  assert(uses[idOf("s")] < uses[idOf("L")]);

  // The coloring is valid: no two interfering values share a color.
  for (int a = 0; a < G.n; ++a)
    for (int b : G.adj[a])
      if (C.color[a] >= 0 && C.color[b] >= 0)
        assert(C.color[a] != C.color[b] && "two neighbors share a color");

  // No more than K colors were used.
  std::set<int> colorsUsed;
  for (int i = 0; i < V.size(); ++i)
    if (C.color[i] >= 0) colorsUsed.insert(C.color[i]);
  assert(colorsUsed.size() <= kPool.size());

  // After the rewrite, s lives in memory: its definition writes a stack slot and
  // its one use (the add that produces w) reads it back.
  MBlock &e = mf.blocks[0];
  const MInst &defS = e.insts[1];  // mov s, 8  -> mov [rbp-8], 8
  assert(defS.op == MOp::Mov);
  assert(defS.ops[0].kind == MOperand::Kind::Mem);
  bool sawReload = false;
  for (const MInst &in : e.insts)
    for (const MOperand &o : in.ops)
      if (o.kind == MOperand::Kind::Mem) sawReload = true;
  assert(sawReload);

  std::cout << "\nchecks passed\n";
  return 0;
}
