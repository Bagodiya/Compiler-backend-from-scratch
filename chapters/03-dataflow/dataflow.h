// A dataflow framework. Chapter 2 already ran a dataflow analysis without
// calling it one: the dominator computation iterated idom[] to a fixpoint. That
// "initialize, walk the CFG meeting neighbors, repeat until nothing changes"
// shape shows up over and over (liveness, reaching definitions, available
// expressions, constant propagation). So this chapter writes the shape down
// once as a reusable solver, then plugs liveness into it.
//
// Header-only, like the chapters before. I kept the IR data model from chapter
// 2 but dropped the SSA-specific fields (phis, idom, frontiers): liveness works
// on plain reassigned variables and doesn't need dominators. What's new lives
// under "the framework" below.
#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <ostream>
#include <set>
#include <string>
#include <vector>

namespace bc {

// --- the IR (carried over from chapter 2) --------------------------------

enum class Type { Void, I1, I64 };

inline const char *typeName(Type t) {
  switch (t) {
  case Type::Void: return "void";
  case Type::I1:   return "i1";
  case Type::I64:  return "i64";
  }
  return "?";
}

enum class Op { Add, Sub, Mul, ICmpLt, Copy, Br, CondBr, Ret };

inline const char *opName(Op op) {
  switch (op) {
  case Op::Add:    return "add";
  case Op::Sub:    return "sub";
  case Op::Mul:    return "mul";
  case Op::ICmpLt: return "icmp lt";
  case Op::Copy:   return "copy";
  case Op::Br:     return "br";
  case Op::CondBr: return "condbr";
  case Op::Ret:    return "ret";
  }
  return "?";
}

struct Variable {
  std::string name;
  Type type = Type::I64;
};

// An operand: an immediate, a function argument, or a read of a variable.
struct Use {
  enum class Kind { Const, Arg, Var };
  Kind kind = Kind::Const;
  int64_t imm = 0;          // Const
  Variable *var = nullptr;  // Var
  std::string text;         // display string for Const/Arg
};

inline Use cst(int64_t v) {
  Use u;
  u.kind = Use::Kind::Const;
  u.imm = v;
  u.text = std::to_string(v);
  return u;
}
inline Use arg(const std::string &name) {
  Use u;
  u.kind = Use::Kind::Arg;
  u.text = "%" + name;
  return u;
}
inline Use rd(Variable *v) {
  Use u;
  u.kind = Use::Kind::Var;
  u.var = v;
  return u;
}

// A straight-line assignment: dst = op(uses).
struct Assign {
  Variable *dst = nullptr;
  Op op = Op::Add;
  std::vector<Use> uses;
};

struct BasicBlock {
  std::string label;
  std::vector<Assign> code;

  // terminator
  Op term = Op::Ret;
  Use cond;    // CondBr
  Use retVal;  // Ret

  std::vector<BasicBlock *> succs;
  std::vector<BasicBlock *> preds;  // computed by computePreds

  void addAssign(Variable *dst, Op op, std::vector<Use> uses) {
    Assign a;
    a.dst = dst;
    a.op = op;
    a.uses = std::move(uses);
    code.push_back(std::move(a));
  }
  void setBr(BasicBlock *d) {
    term = Op::Br;
    succs = {d};
  }
  void setCondBr(Use c, BasicBlock *t, BasicBlock *f) {
    term = Op::CondBr;
    cond = std::move(c);
    succs = {t, f};
  }
  void setRet(Use v) {
    term = Op::Ret;
    retVal = std::move(v);
  }
};

struct Function {
  std::string name;
  Type retType = Type::Void;
  std::vector<std::pair<std::string, Type>> args;
  std::vector<std::unique_ptr<BasicBlock>> blocks;  // blocks[0] is entry
  std::vector<std::unique_ptr<Variable>> vars;

  void addArg(std::string n, Type t) { args.emplace_back(std::move(n), t); }

  BasicBlock *addBlock(std::string label) {
    auto b = std::make_unique<BasicBlock>();
    b->label = std::move(label);
    BasicBlock *p = b.get();
    blocks.push_back(std::move(b));
    return p;
  }
  Variable *addVar(std::string n, Type t) {
    auto v = std::make_unique<Variable>();
    v->name = std::move(n);
    v->type = t;
    Variable *p = v.get();
    vars.push_back(std::move(v));
    return p;
  }
  BasicBlock *entry() const { return blocks.front().get(); }
};

// Successors live on the terminator; predecessors we invert. Same as chapter 2.
inline void computePreds(Function &fn) {
  for (auto &b : fn.blocks) b->preds.clear();
  for (auto &b : fn.blocks)
    for (BasicBlock *s : b->succs) s->preds.push_back(b.get());
}

// --- the framework -------------------------------------------------------
// A dataflow analysis is four pieces: which way the facts flow, a meet
// operator that combines facts where edges join, a transfer function that says
// what one block does to a fact, and boundary/top values to seed the iteration.
// Spell those out and the solver below is the same for every analysis.

enum class Direction { Forward, Backward };

// `Fact` is whatever the analysis tracks at a program point: a set of live
// variables, a map of constants, a bitvector of expressions. It needs `==`
// (so the solver can tell when it stopped changing) and value semantics.
template <typename Fact> struct DataflowProblem {
  Direction dir = Direction::Forward;
  Fact boundary;  // fact at the open end: entry-in (fwd) or exit-out (bwd)
  Fact top;       // identity for meet; the value interior points start at
  std::function<Fact(const Fact &, const Fact &)> meet;
  std::function<Fact(BasicBlock *, const Fact &)> transfer;
};

template <typename Fact> struct DataflowResult {
  std::map<BasicBlock *, Fact> in;   // fact on entry to the block
  std::map<BasicBlock *, Fact> out;  // fact on exit from the block
};

// One worklist solver, parameterized by the problem. For a forward analysis
// the "input" side of a block is its in-set, built by meeting the out-sets of
// its predecessors; transfer turns that into the out-set. Backward is the same
// with in/out and preds/succs swapped, which is the only place direction shows
// up. We loop until a pass changes nothing.
template <typename Fact>
DataflowResult<Fact> solve(Function &fn, const DataflowProblem<Fact> &p) {
  computePreds(fn);
  bool fwd = p.dir == Direction::Forward;

  DataflowResult<Fact> r;
  for (auto &bp : fn.blocks) {
    r.in[bp.get()] = p.top;
    r.out[bp.get()] = p.top;
  }

  std::vector<BasicBlock *> work;
  for (auto &bp : fn.blocks) work.push_back(bp.get());
  std::set<BasicBlock *> onWork(work.begin(), work.end());

  while (!work.empty()) {
    BasicBlock *b = work.back();
    work.pop_back();
    onWork.erase(b);

    // Combine the facts flowing in from the neighbors on the input side. A
    // block with no such neighbors (entry going forward, an exit going
    // backward) takes the boundary fact instead.
    const std::vector<BasicBlock *> &from = fwd ? b->preds : b->succs;
    Fact input;
    if (from.empty()) {
      input = p.boundary;
    } else {
      input = p.top;
      for (BasicBlock *nb : from)
        input = p.meet(input, fwd ? r.out[nb] : r.in[nb]);
    }

    Fact output = p.transfer(b, input);

    Fact &inSlot = fwd ? r.in[b] : r.out[b];    // the meet result
    Fact &outSlot = fwd ? r.out[b] : r.in[b];   // the transferred result
    inSlot = input;
    if (output != outSlot) {
      outSlot = output;
      // Whoever reads our output side has to be recomputed.
      const std::vector<BasicBlock *> &deps = fwd ? b->succs : b->preds;
      for (BasicBlock *d : deps)
        if (onWork.insert(d).second) work.push_back(d);
    }
  }
  return r;
}

// --- an instance: live-variable analysis ---------------------------------
// A variable is live at a point if some later read can observe its current
// value before the next write. Liveness flows backward (a read makes a value
// live at everything upstream that could reach it) and meets by union (live on
// any outgoing edge means live at the bottom of the block). The transfer for a
// block is the textbook one: in = use[b] | (out - def[b]).
//
// We track named Variables, the things that will compete for registers in
// chapters 6 and 7. Arguments are treated as already-live inputs and aren't
// part of the sets.
using VarSet = std::set<Variable *>;

// use[b]: variables read before any write in b (upward-exposed uses).
// def[b]: variables written anywhere in b. A read counts toward use only if the
// block hasn't already defined the variable above it.
inline void computeUseDef(BasicBlock *b, VarSet &use, VarSet &def) {
  use.clear();
  def.clear();
  auto readVar = [&](const Use &u) {
    if (u.kind == Use::Kind::Var && !def.count(u.var)) use.insert(u.var);
  };
  for (Assign &a : b->code) {
    for (Use &u : a.uses) readVar(u);
    def.insert(a.dst);
  }
  if (b->term == Op::CondBr) readVar(b->cond);
  if (b->term == Op::Ret) readVar(b->retVal);
}

inline DataflowProblem<VarSet> livenessProblem(Function &fn) {
  // Precompute use/def once and let the transfer closure share them. shared_ptr
  // because the closure outlives this function's stack frame.
  auto use = std::make_shared<std::map<BasicBlock *, VarSet>>();
  auto def = std::make_shared<std::map<BasicBlock *, VarSet>>();
  for (auto &bp : fn.blocks)
    computeUseDef(bp.get(), (*use)[bp.get()], (*def)[bp.get()]);

  DataflowProblem<VarSet> p;
  p.dir = Direction::Backward;
  p.boundary = {};  // nothing is live past the function's exit
  p.top = {};       // union's identity is the empty set
  p.meet = [](const VarSet &a, const VarSet &b) {
    VarSet u = a;
    u.insert(b.begin(), b.end());
    return u;
  };
  p.transfer = [use, def](BasicBlock *b, const VarSet &out) {
    VarSet in = (*use)[b];
    for (Variable *v : out)
      if (!(*def)[b].count(v)) in.insert(v);
    return in;
  };
  return p;
}

inline DataflowResult<VarSet> runLiveness(Function &fn) {
  return solve(fn, livenessProblem(fn));
}

// --- printing ------------------------------------------------------------
inline std::string render(const Use &u) {
  switch (u.kind) {
  case Use::Kind::Const: return std::to_string(u.imm);
  case Use::Kind::Arg:   return u.text;             // "%a"
  case Use::Kind::Var:   return "%" + u.var->name;  // "%i"
  }
  return "?";
}

inline std::string renderSet(const VarSet &s) {
  std::vector<std::string> names;
  for (Variable *v : s) names.push_back(v->name);
  std::sort(names.begin(), names.end());
  std::string out = "{";
  for (size_t i = 0; i < names.size(); ++i) {
    if (i) out += ", ";
    out += names[i];
  }
  out += "}";
  return out;
}

inline void printBlock(std::ostream &os, BasicBlock *b) {
  os << b->label << ":\n";
  for (Assign &a : b->code) {
    os << "  %" << a.dst->name << " = " << opName(a.op) << ' ';
    for (size_t i = 0; i < a.uses.size(); ++i) {
      if (i) os << ", ";
      os << render(a.uses[i]);
    }
    os << '\n';
  }
  switch (b->term) {
  case Op::Br:
    os << "  br " << b->succs[0]->label << '\n';
    break;
  case Op::CondBr:
    os << "  condbr " << render(b->cond) << ", " << b->succs[0]->label << ", "
       << b->succs[1]->label << '\n';
    break;
  default:
    os << "  ret " << render(b->retVal) << '\n';
    break;
  }
}

inline void printFunction(std::ostream &os, const Function &fn) {
  os << "fn " << fn.name << '(';
  for (size_t i = 0; i < fn.args.size(); ++i) {
    if (i) os << ", ";
    os << typeName(fn.args[i].second) << " %" << fn.args[i].first;
  }
  os << ") -> " << typeName(fn.retType) << " {\n";
  for (auto &bp : fn.blocks) printBlock(os, bp.get());
  os << "}\n";
}

// The function annotated with live-in above each block and live-out below it.
inline void printLiveness(std::ostream &os, const Function &fn,
                          const DataflowResult<VarSet> &r) {
  os << "fn " << fn.name << " (liveness):\n";
  for (auto &bp : fn.blocks) {
    BasicBlock *b = bp.get();
    os << "  live-in  " << renderSet(r.in.at(b)) << '\n';
    printBlock(os, b);
    os << "  live-out " << renderSet(r.out.at(b)) << "\n\n";
  }
}

} // namespace bc
