// Optimizations that ride on the dataflow framework from chapter 3. The story
// here is a small pipeline: figure out which values are compile-time constants,
// rewrite the code to use those constants (constant folding), then delete the
// instructions nobody reads anymore (dead-code elimination). The first step is
// just another instance of the chapter 3 solver, this time with a lattice fact
// instead of a set. The last step finally *uses* the liveness we computed last
// chapter for something concrete.
//
// SCCP, GVN, and LICM are the rest of the optimization family and I sketch them
// in the README, but they each want machinery we either don't have yet (SCCP
// wants edge reachability folded into the same pass) or that belongs in its own
// chapter (GVN's value numbering, LICM's loop+dominator analysis). So this file
// builds the three that fall straight out of what we already have.
//
// Header-only and standalone, like every chapter: the IR, framework, and
// liveness below are copied from chapter 3 unchanged. Everything new lives under
// "constant propagation" and after.
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

// --- the IR (carried over from chapter 3) --------------------------------

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

// Successors live on the terminator; predecessors we invert. Same as chapter 3.
inline void computePreds(Function &fn) {
  for (auto &b : fn.blocks) b->preds.clear();
  for (auto &b : fn.blocks)
    for (BasicBlock *s : b->succs) s->preds.push_back(b.get());
}

// --- the framework (carried over from chapter 3) -------------------------

enum class Direction { Forward, Backward };

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
      const std::vector<BasicBlock *> &deps = fwd ? b->succs : b->preds;
      for (BasicBlock *d : deps)
        if (onWork.insert(d).second) work.push_back(d);
    }
  }
  return r;
}

// --- liveness (carried over from chapter 3) ------------------------------

using VarSet = std::set<Variable *>;

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
  auto use = std::make_shared<std::map<BasicBlock *, VarSet>>();
  auto def = std::make_shared<std::map<BasicBlock *, VarSet>>();
  for (auto &bp : fn.blocks)
    computeUseDef(bp.get(), (*use)[bp.get()], (*def)[bp.get()]);

  DataflowProblem<VarSet> p;
  p.dir = Direction::Backward;
  p.boundary = {};
  p.top = {};
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

// --- constant propagation (new) ------------------------------------------
// We want to know, at every program point, which variables hold a value the
// compiler can pin down. The fact per variable is a point on a tiny lattice:
//
//   top      -- haven't seen a definition reach here yet (optimistic)
//   Const(c) -- definitely the constant c on every path that gets here
//   bottom   -- two paths disagree, or it came from something unknown (an arg)
//
// Meet pulls values *down* this lattice where edges join. top is the identity
// (it's also the chapter 3 `top`), bottom is absorbing, and two different
// constants meet to bottom. That monotone "only ever move down a finite
// lattice" property is exactly what makes the chapter 3 solver terminate.

struct CV {
  enum class K { Top, Const, Bottom };
  K k = K::Top;
  int64_t val = 0;
};

inline bool operator==(const CV &a, const CV &b) {
  return a.k == b.k && (a.k != CV::K::Const || a.val == b.val);
}
inline bool operator!=(const CV &a, const CV &b) { return !(a == b); }

inline CV cvTop() { return CV{CV::K::Top, 0}; }
inline CV cvConst(int64_t v) { return CV{CV::K::Const, v}; }
inline CV cvBot() { return CV{CV::K::Bottom, 0}; }

// The fact is a map from variable to lattice point. To keep `==` honest we keep
// it canonical: a variable at top is simply absent, so two maps are equal iff
// they agree on every constant-or-bottom variable.
using ConstMap = std::map<Variable *, CV>;

inline CV cvGet(const ConstMap &m, Variable *v) {
  auto it = m.find(v);
  return it == m.end() ? cvTop() : it->second;
}
inline void cvSet(ConstMap &m, Variable *v, CV c) {
  if (c.k == CV::K::Top) m.erase(v);
  else m[v] = c;
}

inline CV cvMeet(CV a, CV b) {
  if (a.k == CV::K::Top) return b;
  if (b.k == CV::K::Top) return a;
  if (a.k == CV::K::Bottom || b.k == CV::K::Bottom) return cvBot();
  return a.val == b.val ? a : cvBot();  // two constants: agree or give up
}

inline CV cvOfUse(const ConstMap &m, const Use &u) {
  switch (u.kind) {
  case Use::Kind::Const: return cvConst(u.imm);
  case Use::Kind::Arg:   return cvBot();  // unknown until run time
  case Use::Kind::Var:   return cvGet(m, u.var);
  }
  return cvBot();
}

// Evaluate the right-hand side of an assignment over the lattice. If an operand
// is still top we stay top (wait for it); if any is bottom the result is
// bottom; only when every operand is a concrete constant do we actually fold.
inline CV cvOfAssign(const ConstMap &m, const Assign &a) {
  if (a.op == Op::Copy) return cvOfUse(m, a.uses[0]);

  CV x = cvOfUse(m, a.uses[0]);
  CV y = cvOfUse(m, a.uses[1]);
  if (x.k == CV::K::Top || y.k == CV::K::Top) return cvTop();
  if (x.k == CV::K::Bottom || y.k == CV::K::Bottom) return cvBot();

  int64_t r = 0;
  switch (a.op) {
  case Op::Add:    r = x.val + y.val; break;
  case Op::Sub:    r = x.val - y.val; break;
  case Op::Mul:    r = x.val * y.val; break;
  case Op::ICmpLt: r = (x.val < y.val) ? 1 : 0; break;
  default:         return cvBot();
  }
  return cvConst(r);
}

// Constant propagation is a forward, meet-by-lattice problem. The transfer for
// a block threads the running map through its assignments, the same way the
// folding rewrite below will.
inline DataflowProblem<ConstMap> constPropProblem() {
  DataflowProblem<ConstMap> p;
  p.dir = Direction::Forward;
  p.boundary = {};  // at entry every local is still top (undefined)
  p.top = {};
  p.meet = [](const ConstMap &a, const ConstMap &b) {
    ConstMap r;
    for (auto &kv : a) {
      CV m = cvMeet(kv.second, cvGet(b, kv.first));
      if (m.k != CV::K::Top) r[kv.first] = m;
    }
    for (auto &kv : b)  // keys only in b meet with top, i.e. stay themselves
      if (!a.count(kv.first)) r[kv.first] = kv.second;
    return r;
  };
  p.transfer = [](BasicBlock *b, const ConstMap &in) {
    ConstMap m = in;
    for (Assign &a : b->code) cvSet(m, a.dst, cvOfAssign(m, a));
    return m;
  };
  return p;
}

inline DataflowResult<ConstMap> runConstProp(Function &fn) {
  return solve(fn, constPropProblem());
}

// --- constant folding (new) ----------------------------------------------
// The analysis above only computes facts; this is where we rewrite the code.
// For each block we replay the lattice from its in-fact, instruction by
// instruction. Two rewrites: any operand that reads a known-constant variable
// becomes an immediate, and any assignment whose result is constant collapses
// to a plain `copy` of that constant. The collapse is what frees the inputs to
// be deleted later -- a folded `c = mul a, b` stops reading a and b at all.
inline void foldConstants(Function &fn) {
  DataflowResult<ConstMap> res = runConstProp(fn);

  for (auto &bp : fn.blocks) {
    BasicBlock *b = bp.get();
    ConstMap m = res.in.at(b);

    auto foldUse = [&](Use &u) {
      if (u.kind == Use::Kind::Var) {
        CV c = cvGet(m, u.var);
        if (c.k == CV::K::Const) u = cst(c.val);
      }
    };

    for (Assign &a : b->code) {
      for (Use &u : a.uses) foldUse(u);
      CV r = cvOfAssign(m, a);
      if (r.k == CV::K::Const) {
        a.op = Op::Copy;
        a.uses = {cst(r.val)};
      }
      cvSet(m, a.dst, r);  // keep the replay in sync for later instructions
    }
    if (b->term == Op::CondBr) foldUse(b->cond);
    if (b->term == Op::Ret) foldUse(b->retVal);
  }
}

// --- dead-code elimination (new) -----------------------------------------
// Every op in this IR is pure, so an assignment is dead exactly when its result
// is not live afterward. That's the liveness from chapter 3, finally cashed in.
// We walk each block backward from its live-out set: drop any assignment whose
// destination isn't live at that point, and for the ones we keep, fold their
// reads back into the live set. The backward sweep cascades within a block on
// its own; we loop the whole thing because deleting a use in one block can make
// a definition in another block dead on the next round.
inline void deadCodeElim(Function &fn) {
  bool changed = true;
  while (changed) {
    changed = false;
    DataflowResult<VarSet> live = runLiveness(fn);

    for (auto &bp : fn.blocks) {
      BasicBlock *b = bp.get();
      VarSet liveNow = live.out.at(b);
      // The terminator reads come after all the code, so seed them first or the
      // backward sweep would think the condition (or return value) is dead.
      if (b->term == Op::CondBr && b->cond.kind == Use::Kind::Var)
        liveNow.insert(b->cond.var);
      if (b->term == Op::Ret && b->retVal.kind == Use::Kind::Var)
        liveNow.insert(b->retVal.var);
      std::vector<Assign> kept;  // collected in reverse, flipped at the end

      for (auto it = b->code.rbegin(); it != b->code.rend(); ++it) {
        Assign &a = *it;
        if (!liveNow.count(a.dst)) {
          changed = true;  // dead store, drop it
          continue;
        }
        liveNow.erase(a.dst);
        for (Use &u : a.uses)
          if (u.kind == Use::Kind::Var) liveNow.insert(u.var);
        kept.push_back(a);
      }

      std::reverse(kept.begin(), kept.end());
      b->code = std::move(kept);
    }
  }
}

// --- printing (carried over from chapter 3, plus a lattice dump) ----------
inline std::string render(const Use &u) {
  switch (u.kind) {
  case Use::Kind::Const: return std::to_string(u.imm);
  case Use::Kind::Arg:   return u.text;             // "%a"
  case Use::Kind::Var:   return "%" + u.var->name;  // "%i"
  }
  return "?";
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

inline std::string renderCV(CV c) {
  switch (c.k) {
  case CV::K::Top:    return "top";
  case CV::K::Const:  return std::to_string(c.val);
  case CV::K::Bottom: return "bot";
  }
  return "?";
}

// Per block, the constants known on exit. A quick way to eyeball the analysis
// before folding touches anything.
inline void printConstProp(std::ostream &os, const Function &fn,
                           const DataflowResult<ConstMap> &res) {
  os << "fn " << fn.name << " (constants on block exit):\n";
  for (auto &bp : fn.blocks) {
    BasicBlock *b = bp.get();
    os << "  " << b->label << ": {";
    bool first = true;
    for (auto &kv : res.out.at(b)) {
      if (kv.second.k != CV::K::Const) continue;
      if (!first) os << ", ";
      os << kv.first->name << "=" << renderCV(kv.second);
      first = false;
    }
    os << "}\n";
  }
}

} // namespace bc
