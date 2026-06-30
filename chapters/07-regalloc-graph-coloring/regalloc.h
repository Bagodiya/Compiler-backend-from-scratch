// Register allocation, part 2: graph coloring.
//
// Linear scan (chapter 6) swept the instructions once, left to right, and made
// each spill decision on the spot. It's fast and the result is fine, but it's
// greedy: it never sees the whole picture, and its spill rule -- evict whatever
// reaches furthest -- is purely local.
//
// Graph coloring takes the opposite stance. It builds one global object, the
// interference graph, where the nodes are values and an edge means "these two are
// live at the same time, so they can't share a register." Allocating registers is
// then exactly the problem of coloring that graph with K colors, K being the
// number of physical registers. That global view is what lets it pick a smarter
// value to spill and, later, fold away copies (coalescing). It costs more, which
// is why a JIT reaches for linear scan and an ahead-of-time compiler reaches for
// this.
//
// I carry the IR core, the machine layer, and the selector forward from chapter
// 6 unchanged. What's new sits at the bottom: a small liveness pass, the
// interference graph it feeds, and the simplify/select coloring algorithm
// (Chaitin-Briggs, with optimistic spilling).
#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <ostream>
#include <set>
#include <string>
#include <vector>

namespace bc {

// --- the IR (carried over from chapter 6) --------------------------------

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
  std::string text;         // display string for Const/Arg ("%a" for an arg)
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

// --- the machine layer (carried over from chapter 6) ---------------------

enum class MOp { Mov, Add, Sub, IMul, Cmp, Setl, Jmp, Jl, Jne, Ret };

inline const char *mopName(MOp op) {
  switch (op) {
  case MOp::Mov:  return "mov";
  case MOp::Add:  return "add";
  case MOp::Sub:  return "sub";
  case MOp::IMul: return "imul";
  case MOp::Cmp:  return "cmp";
  case MOp::Setl: return "setl";
  case MOp::Jmp:  return "jmp";
  case MOp::Jl:   return "jl";
  case MOp::Jne:  return "jne";
  case MOp::Ret:  return "ret";
  }
  return "?";
}

struct MOperand {
  enum class Kind { Reg, Imm, Label, Mem };
  Kind kind = Kind::Imm;
  int reg = 0;          // Reg: virtual register id
  std::string name;     // Reg: its name; Label: target; Mem: address text
  int64_t imm = 0;      // Imm
};

inline MOperand mreg(int id, std::string name) {
  MOperand o;
  o.kind = MOperand::Kind::Reg;
  o.reg = id;
  o.name = std::move(name);
  return o;
}
inline MOperand mimm(int64_t v) {
  MOperand o;
  o.kind = MOperand::Kind::Imm;
  o.imm = v;
  return o;
}
inline MOperand mlabel(std::string l) {
  MOperand o;
  o.kind = MOperand::Kind::Label;
  o.name = std::move(l);
  return o;
}

struct MInst {
  MOp op;
  std::vector<MOperand> ops;  // destination first, x86 (Intel) order
};

struct MBlock {
  std::string label;
  std::vector<MInst> insts;
};

struct MFunction {
  std::string name;
  std::vector<MBlock> blocks;
};

inline void emit(MBlock &mb, MOp op, std::vector<MOperand> ops) {
  mb.insts.push_back(MInst{op, std::move(ops)});
}

// --- the selector (carried over from chapter 6) --------------------------

struct Selector {
  std::map<Variable *, int> varReg;
  std::map<std::string, int> argReg;
  std::vector<std::string> regName;  // indexed by virtual register id

  int fresh(const std::string &name) {
    int id = static_cast<int>(regName.size());
    regName.push_back(name);
    return id;
  }
  MOperand regOp(int id) { return mreg(id, regName[id]); }

  MOperand regForVar(Variable *v) {
    auto it = varReg.find(v);
    if (it != varReg.end()) return regOp(it->second);
    int id = fresh(v->name);
    varReg[v] = id;
    return regOp(id);
  }
  MOperand regForArg(const std::string &name) {
    auto it = argReg.find(name);
    if (it != argReg.end()) return regOp(it->second);
    int id = fresh(name);
    argReg[name] = id;
    return regOp(id);
  }

  MOperand operandOf(const Use &u) {
    switch (u.kind) {
    case Use::Kind::Const: return mimm(u.imm);
    case Use::Kind::Arg:   return regForArg(u.text.substr(1));  // "%a" -> "a"
    case Use::Kind::Var:   return regForVar(u.var);
    }
    return mimm(0);
  }

  MOperand intoReg(MOperand o, MBlock &mb) {
    if (o.kind == MOperand::Kind::Reg) return o;
    MOperand r = regOp(fresh("t" + std::to_string(regName.size())));
    emit(mb, MOp::Mov, {r, o});
    return r;
  }

  void selectAssign(const Assign &a, MBlock &mb) {
    switch (a.op) {
    case Op::Copy:
      emit(mb, MOp::Mov, {regForVar(a.dst), operandOf(a.uses[0])});
      break;
    case Op::Add:
    case Op::Sub:
    case Op::Mul: {
      MOperand dst = regForVar(a.dst);
      emit(mb, MOp::Mov, {dst, operandOf(a.uses[0])});
      MOperand rhs = operandOf(a.uses[1]);
      MOp m = a.op == Op::Add ? MOp::Add : a.op == Op::Sub ? MOp::Sub : MOp::IMul;
      if (m == MOp::IMul) rhs = intoReg(rhs, mb);  // imul wants a register source
      emit(mb, m, {dst, rhs});
      break;
    }
    case Op::ICmpLt: {
      MOperand lhs = intoReg(operandOf(a.uses[0]), mb);
      emit(mb, MOp::Cmp, {lhs, operandOf(a.uses[1])});
      emit(mb, MOp::Setl, {regForVar(a.dst)});
      break;
    }
    default:
      break;
    }
  }

  void selectBlock(BasicBlock *b, MBlock &mb,
                   const std::map<Variable *, int> &reads) {
    size_t n = b->code.size();

    bool fuse = false;
    if (b->term == Op::CondBr && b->cond.kind == Use::Kind::Var && n > 0) {
      const Assign &last = b->code[n - 1];
      auto it = reads.find(b->cond.var);
      if (last.op == Op::ICmpLt && last.dst == b->cond.var &&
          it != reads.end() && it->second == 1)
        fuse = true;
    }

    size_t emitN = fuse ? n - 1 : n;
    for (size_t i = 0; i < emitN; ++i) selectAssign(b->code[i], mb);

    switch (b->term) {
    case Op::Br:
      emit(mb, MOp::Jmp, {mlabel(b->succs[0]->label)});
      break;
    case Op::CondBr:
      if (fuse) {
        const Assign &cmp = b->code[n - 1];
        MOperand lhs = intoReg(operandOf(cmp.uses[0]), mb);
        emit(mb, MOp::Cmp, {lhs, operandOf(cmp.uses[1])});
        emit(mb, MOp::Jl, {mlabel(b->succs[0]->label)});
      } else {
        emit(mb, MOp::Cmp, {operandOf(b->cond), mimm(0)});
        emit(mb, MOp::Jne, {mlabel(b->succs[0]->label)});
      }
      emit(mb, MOp::Jmp, {mlabel(b->succs[1]->label)});
      break;
    default:  // Ret
      emit(mb, MOp::Ret, {operandOf(b->retVal)});
      break;
    }
  }
};

inline std::map<Variable *, int> countReads(const Function &fn) {
  std::map<Variable *, int> r;
  auto rd = [&](const Use &u) {
    if (u.kind == Use::Kind::Var) r[u.var]++;
  };
  for (auto &bp : fn.blocks) {
    for (const Assign &a : bp->code)
      for (const Use &u : a.uses) rd(u);
    if (bp->term == Op::CondBr) rd(bp->cond);
    if (bp->term == Op::Ret) rd(bp->retVal);
  }
  return r;
}

inline MFunction selectFunction(const Function &fn) {
  Selector sel;
  for (auto &a : fn.args) sel.regForArg(a.first);

  std::map<Variable *, int> reads = countReads(fn);

  MFunction mf;
  mf.name = fn.name;
  for (auto &bp : fn.blocks) {
    MBlock mb;
    mb.label = bp->label;
    sel.selectBlock(bp.get(), mb, reads);
    mf.blocks.push_back(std::move(mb));
  }
  return mf;
}

// --- values (new) --------------------------------------------------------
// Both the liveness pass and the interference graph want to talk about "values"
// uniformly, whether they came from a variable or a function argument. So I give
// every one a small integer id and remember its name for printing. Populate the
// registry once up front so later lookups never invent a new id.

struct Values {
  std::vector<std::string> name;     // id -> display name
  std::map<Variable *, int> varId;
  std::map<std::string, int> argId;  // by argument name

  int forVar(Variable *v) {
    auto it = varId.find(v);
    if (it != varId.end()) return it->second;
    int id = static_cast<int>(name.size());
    name.push_back(v->name);
    varId[v] = id;
    return id;
  }
  int forArg(const std::string &a) {
    auto it = argId.find(a);
    if (it != argId.end()) return it->second;
    int id = static_cast<int>(name.size());
    name.push_back(a);
    argId[a] = id;
    return id;
  }
  // The value an operand reads, or -1 for a constant.
  int idOf(const Use &u) {
    if (u.kind == Use::Kind::Var) return forVar(u.var);
    if (u.kind == Use::Kind::Arg) return forArg(u.text.substr(1));
    return -1;
  }
  int size() const { return static_cast<int>(name.size()); }
};

inline Values collectValues(Function &fn) {
  Values V;
  for (auto &a : fn.args) V.forArg(a.first);
  for (auto &bp : fn.blocks) {
    for (Assign &a : bp->code) {
      for (Use &u : a.uses) V.idOf(u);
      if (a.dst) V.forVar(a.dst);
    }
    if (bp->term == Op::CondBr) V.idOf(bp->cond);
    if (bp->term == Op::Ret) V.idOf(bp->retVal);
  }
  return V;
}

// How many times each value is read. This is the cheap stand-in for spill cost:
// a value read once is cheap to keep in memory, a value read a lot is not.
inline std::vector<int> countUses(Function &fn, Values &V) {
  std::vector<int> uses(V.size(), 0);
  auto rd = [&](const Use &u) {
    int id = V.idOf(u);
    if (id >= 0) uses[id]++;
  };
  for (auto &bp : fn.blocks) {
    for (Assign &a : bp->code)
      for (Use &u : a.uses) rd(u);
    if (bp->term == Op::CondBr) rd(bp->cond);
    if (bp->term == Op::Ret) rd(bp->retVal);
  }
  return uses;
}

// --- liveness (new) ------------------------------------------------------
// A value is live at a point if it will be read again before it's overwritten.
// This is the same backward dataflow we built in chapter 3, specialized to one
// fact (liveness) and one direction (backward). For each block I work out the
// upward-exposed uses (read before any local definition) and the definitions,
// then iterate the two equations to a fixed point:
//
//     live_out[B] = union of live_in over B's successors
//     live_in[B]  = uses[B]  union  (live_out[B] minus defs[B])

struct Liveness {
  std::map<BasicBlock *, std::set<int>> in, out;
};

inline Liveness computeLiveness(Function &fn, Values &V) {
  std::map<BasicBlock *, std::set<int>> use, def;
  for (auto &bp : fn.blocks) {
    BasicBlock *b = bp.get();
    std::set<int> defined;  // grows as we scan the block top to bottom
    auto rd = [&](const Use &u) {
      int id = V.idOf(u);
      if (id >= 0 && !defined.count(id)) use[b].insert(id);  // read before def
    };
    for (Assign &a : b->code) {
      for (Use &u : a.uses) rd(u);
      if (a.dst) defined.insert(V.forVar(a.dst));
    }
    if (b->term == Op::CondBr) rd(b->cond);
    if (b->term == Op::Ret) rd(b->retVal);
    def[b] = defined;
  }

  Liveness L;
  bool changed = true;
  while (changed) {
    changed = false;
    // Walk blocks back to front; it converges faster but any order works.
    for (auto it = fn.blocks.rbegin(); it != fn.blocks.rend(); ++it) {
      BasicBlock *b = it->get();
      std::set<int> out;
      for (BasicBlock *s : b->succs)
        out.insert(L.in[s].begin(), L.in[s].end());
      std::set<int> in = use[b];
      for (int x : out)
        if (!def[b].count(x)) in.insert(x);
      if (in != L.in[b] || out != L.out[b]) {
        L.in[b] = std::move(in);
        L.out[b] = std::move(out);
        changed = true;
      }
    }
  }
  return L;
}

// --- the interference graph (new) ----------------------------------------
// One node per value, an undirected edge between any two that are ever live at
// the same time. The construction is the textbook one: walk each block backward
// from its live-out set, and at every definition draw an edge from the defined
// value to everything still live. A value's own operands aren't yet live when it
// is defined (they were read, not held), so they don't get spurious edges.

struct Graph {
  int n = 0;
  std::vector<std::set<int>> adj;

  void addEdge(int a, int b) {
    if (a == b) return;
    adj[a].insert(b);
    adj[b].insert(a);
  }
  int degree(int v) const { return static_cast<int>(adj[v].size()); }
};

inline Graph buildInterference(Function &fn, Values &V, const Liveness &L) {
  Graph G;
  G.n = V.size();
  G.adj.resize(G.n);

  for (auto &bp : fn.blocks) {
    BasicBlock *b = bp.get();
    std::set<int> live = L.out.at(b);
    auto addLive = [&](const Use &u) {
      int id = V.idOf(u);
      if (id >= 0) live.insert(id);
    };
    // The terminator runs after the last assignment, so fold its reads in first.
    if (b->term == Op::CondBr) addLive(b->cond);
    if (b->term == Op::Ret) addLive(b->retVal);

    for (auto it = b->code.rbegin(); it != b->code.rend(); ++it) {
      const Assign &a = *it;
      if (a.dst) {
        int d = V.forVar(a.dst);
        for (int w : live)
          G.addEdge(d, w);
        // One machine-shaped wrinkle. x86 arithmetic is two-address, so the
        // selector lowers `d = u op v` as `mov d, u; op d, v`. That writes d
        // before reading v, so d may safely reuse u's register but must not land
        // on v's. Record that as interference between the result and the second
        // operand. (Copies and compares don't have this hazard.)
        if ((a.op == Op::Add || a.op == Op::Sub || a.op == Op::Mul) &&
            a.uses.size() >= 2) {
          int second = V.idOf(a.uses[1]);
          if (second >= 0) G.addEdge(d, second);
        }
        live.erase(d);
      }
      for (const Use &u : a.uses) {
        int id = V.idOf(u);
        if (id >= 0) live.insert(id);
      }
    }
  }

  // Arguments are all defined together at entry, so any two that are live there
  // interfere. (No effect when the entry block has no live-in, as ours won't.)
  const std::set<int> &entryLive = L.in.at(fn.entry());
  for (int a : entryLive)
    for (int b : entryLive)
      if (a < b) G.addEdge(a, b);

  return G;
}

// --- graph coloring (new) ------------------------------------------------
// Chaitin-Briggs in two halves.
//
// Simplify: repeatedly pull off a node with fewer than K neighbors and push it on
// a stack. Such a node is trivially colorable -- whatever its neighbors take,
// there's a color left over -- so we can defer it and shrink the graph. When
// every remaining node has K or more neighbors we're stuck, so we pick the
// cheapest node to spill (fewest uses, breaking ties toward higher degree, which
// frees up more of the graph) and push it as an optimistic spill candidate.
//
// Select: pop the stack and give each node a color none of its already-colored
// neighbors use. A regular node always finds one. An optimistic candidate might
// too -- if its neighbors happened to collapse onto fewer than K colors -- and
// then we got a register for free. If it can't, it becomes a real spill.

struct Coloring {
  std::vector<int> color;     // value id -> color index, or -1
  std::set<int> spilled;
  std::vector<int> order;     // simplify push order (bottom first)
  std::set<int> optimistic;   // ids pushed as spill candidates
};

inline Coloring colorGraph(const Graph &G, int K, const std::vector<int> &uses) {
  std::vector<bool> removed(G.n, false);
  Coloring C;
  C.color.assign(G.n, -1);

  auto liveDegree = [&](int v) {
    int d = 0;
    for (int w : G.adj[v])
      if (!removed[w]) ++d;
    return d;
  };

  int left = G.n;
  while (left > 0) {
    int pick = -1;
    for (int v = 0; v < G.n; ++v)
      if (!removed[v] && liveDegree(v) < K) {
        pick = v;
        break;
      }
    if (pick < 0) {
      // Stuck: choose the spill candidate. Cheapest first (fewest uses), then
      // the one wired to the most still-present nodes.
      for (int v = 0; v < G.n; ++v) {
        if (removed[v]) continue;
        if (pick < 0 || uses[v] < uses[pick] ||
            (uses[v] == uses[pick] && liveDegree(v) > liveDegree(pick)))
          pick = v;
      }
      C.optimistic.insert(pick);
    }
    removed[pick] = true;
    C.order.push_back(pick);
    --left;
  }

  for (auto it = C.order.rbegin(); it != C.order.rend(); ++it) {
    int v = *it;
    std::set<int> taken;
    for (int w : G.adj[v])
      if (C.color[w] >= 0) taken.insert(C.color[w]);
    int chosen = -1;
    for (int c = 0; c < K; ++c)
      if (!taken.count(c)) {
        chosen = c;
        break;
      }
    if (chosen < 0)
      C.spilled.insert(v);  // optimism didn't pan out
    else
      C.color[v] = chosen;
  }
  return C;
}

// Rewrite the machine code in place. Every virtual register carries the name of
// the value it holds, so we look the decision up by name: a physical register, or
// a stack slot for a spilled value.
//
// Same honest caveat as chapter 6: dropping a spilled value straight into a
// memory operand is only legal because our spilled value is touched by `mov` and
// `add`, both of which take a memory operand. A spill that feeds `imul` would need
// a reload into a scratch register first; that lowering waits for chapter 9.
inline void applyColoring(MFunction &mf, const Values &V, const Coloring &C,
                          const std::vector<std::string> &pool) {
  std::map<std::string, std::string> reg;  // value name -> physical register
  std::map<std::string, int> slot;         // value name -> stack slot
  int nextSlot = 0;
  for (int id = 0; id < V.size(); ++id) {
    if (C.spilled.count(id))
      slot[V.name[id]] = nextSlot++;
    else if (C.color[id] >= 0)
      reg[V.name[id]] = pool[C.color[id]];
  }
  for (MBlock &b : mf.blocks)
    for (MInst &in : b.insts)
      for (MOperand &o : in.ops) {
        if (o.kind != MOperand::Kind::Reg) continue;
        auto s = slot.find(o.name);
        if (s != slot.end()) {
          o.kind = MOperand::Kind::Mem;
          o.name = "rbp-" + std::to_string((s->second + 1) * 8);
        } else {
          o.name = reg.at(o.name);
        }
      }
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

inline std::string renderMOperand(const MOperand &o) {
  switch (o.kind) {
  case MOperand::Kind::Reg:   return "%" + o.name;
  case MOperand::Kind::Imm:   return std::to_string(o.imm);
  case MOperand::Kind::Label: return o.name;
  case MOperand::Kind::Mem:   return "[" + o.name + "]";
  }
  return "?";
}

inline void printMFunction(std::ostream &os, const MFunction &mf) {
  os << mf.name << ":\n";
  for (const MBlock &b : mf.blocks) {
    os << b.label << ":\n";
    for (const MInst &i : b.insts) {
      os << "  " << mopName(i.op);
      for (size_t k = 0; k < i.ops.size(); ++k)
        os << (k ? ", " : " ") << renderMOperand(i.ops[k]);
      os << '\n';
    }
  }
}

inline void printInterference(std::ostream &os, const Values &V, const Graph &G) {
  os << "interference graph (an edge = cannot share a register):\n";
  for (int v = 0; v < G.n; ++v) {
    os << "  " << V.name[v] << ":";
    for (int w : G.adj[v]) os << ' ' << V.name[w];
    os << "\n";
  }
}

inline void printColoring(std::ostream &os, const Values &V, const Coloring &C,
                          const std::vector<std::string> &pool) {
  os << "coloring (K = " << pool.size() << "):\n";
  int nextSlot = 0;
  for (int v = 0; v < V.size(); ++v) {
    os << "  " << V.name[v] << " -> ";
    if (C.spilled.count(v))
      os << "[rbp-" << (nextSlot++ + 1) * 8 << "]  (spilled)\n";
    else
      os << "%" << pool[C.color[v]] << "\n";
  }
}

} // namespace bc
