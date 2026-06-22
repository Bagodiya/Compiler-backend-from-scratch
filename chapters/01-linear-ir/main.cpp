#include "ir.h"

#include <cassert>
#include <iostream>
#include <sstream>

using namespace bc;

// Build this by hand and print it:
//
//   fn f(a, b) {
//     t = (a + b) * 2
//     if t < 100 -> small else -> big
//     small: return t
//     big:   return t - 100
//   }
//
// A frontend would build this from an AST. We're doing the backend, so we start
// straight at the IR.
int main() {
  Function fn;
  fn.name = "f";
  fn.retType = Type::I64;
  Argument *a = fn.addArg(Type::I64, "a");
  Argument *b = fn.addArg(Type::I64, "b");

  BasicBlock *entry = fn.addBlock("entry");
  BasicBlock *small = fn.addBlock("small");
  BasicBlock *big = fn.addBlock("big");

  IRBuilder B(fn);

  B.setBlock(entry);
  Value *t = B.mul(B.add(a, b), B.constant(2));
  B.condBr(B.icmpLt(t, B.constant(100)), small, big);

  B.setBlock(small);
  B.ret(t);

  B.setBlock(big);
  B.ret(B.sub(t, B.constant(100)));

  print(std::cout, fn);

  // a couple of sanity checks
  std::ostringstream os;
  print(os, fn);
  std::string text = os.str();
  assert(text.find("icmp lt") != std::string::npos);
  assert(entry->terminator() && entry->terminator()->op == Op::CondBr);
  assert(entry->terminator()->succs.size() == 2);
  assert(small->terminator() && small->terminator()->op == Op::Ret);

  std::cout << "\nchecks passed\n";
  return 0;
}
