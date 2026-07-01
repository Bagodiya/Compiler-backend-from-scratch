// Scheduling and peephole.
//
// By now the selector (chapter 5) has handed us a list of machine instructions
// and the allocators (chapters 6-7) have put every value in a register or on the
// stack. The order of those instructions is still just the order the selector
// happened to emit them, and it's full of tiny local waste. This chapter cleans
// up both.
//
// Two passes. The first is instruction scheduling: reorder the instructions in a
// block so a long-latency operation (a multiply, say) starts early and its result
// is ready by the time something needs it, instead of stalling the pipeline. I run
// it as a post-pass over the allocated code, on physical registers, so the register
// reuse the allocator introduced shows up honestly as anti- and output-dependences
// the scheduler won't cross. Reorder before allocation instead and you have to redo
// liveness on the new order, or the code quietly computes garbage -- see the note on
// the scheduling pass below for the bug that taught me this. The second is peephole:
// a sliding window over the final stream that rewrites patterns the earlier stages
// don't bother to avoid, like the self-moves chapter 7 pointed at.
//
// I carry the IR core, the machine layer, the selector, and the whole graph-
// coloring allocator forward from chapter 7 unchanged. What's new sits at the
// bottom: the dependence DAG, a list scheduler, and the peephole pass.
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

// --- the IR (carried over from chapter 7) --------------------------------

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

// --- the machine layer (carried over from chapter 7) ---------------------

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

// --- the selector (carried over from chapter 7) --------------------------

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

// --- values (carried over from chapter 7) --------------------------------

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

// --- liveness (carried over from chapter 7) ------------------------------

struct Liveness {
  std::map<BasicBlock *, std::set<int>> in, out;
};

inline Liveness computeLiveness(Function &fn, Values &V) {
  std::map<BasicBlock *, std::set<int>> use, def;
  for (auto &bp : fn.blocks) {
    BasicBlock *b = bp.get();
    std::set<int> defined;
    auto rd = [&](const Use &u) {
      int id = V.idOf(u);
      if (id >= 0 && !defined.count(id)) use[b].insert(id);
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

// --- the interference graph (carried over from chapter 7) ----------------

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
    if (b->term == Op::CondBr) addLive(b->cond);
    if (b->term == Op::Ret) addLive(b->retVal);

    for (auto it = b->code.rbegin(); it != b->code.rend(); ++it) {
      const Assign &a = *it;
      if (a.dst) {
        int d = V.forVar(a.dst);
        for (int w : live)
          G.addEdge(d, w);
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

  const std::set<int> &entryLive = L.in.at(fn.entry());
  for (int a : entryLive)
    for (int b : entryLive)
      if (a < b) G.addEdge(a, b);

  return G;
}

// --- graph coloring (carried over from chapter 7) ------------------------

struct Coloring {
  std::vector<int> color;
  std::set<int> spilled;
  std::vector<int> order;
  std::set<int> optimistic;
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
      C.spilled.insert(v);
    else
      C.color[v] = chosen;
  }
  return C;
}

inline void applyColoring(MFunction &mf, const Values &V, const Coloring &C,
                          const std::vector<std::string> &pool) {
  std::map<std::string, std::string> reg;
  std::map<std::string, int> slot;
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

// --- instruction scheduling (new) ----------------------------------------
// I run this as a post-pass on the allocated code, over physical registers. The
// freest place to schedule is before allocation, when every value still has its own
// register and the only edges are real data dependences -- but reorder there and the
// allocator, which computes liveness from the (still unscheduled) IR, colors the
// wrong live ranges and the code quietly computes garbage. I got that wrong the
// first time: a multiply hoisted over an add that shared its register clobbered a
// value the add still needed. Scheduling after keeps the pass honest -- the reuse
// the allocator introduced shows up as extra anti- and output-dependences, and
// refusing to cross them is what keeps the reorder legal. The cost is less freedom,
// which is the classic tension between these two passes.
//
// A dependence edge i -> j (i earlier) exists when the two instructions touch a
// common location the wrong way round: j reads what i writes (true/RAW), j writes
// what i reads (anti/WAR), or both write it (output/WAW). Preserve every such edge
// and any order you emit computes the same result. Because I only ever draw edges
// from a lower index to a higher one, the original program order is itself a
// topological order, which makes the critical-path math a single backward pass.

inline int latency(MOp op) {
  // A stand-in for a real pipeline model: multiply is the one slow op here.
  return op == MOp::IMul ? 3 : 1;
}

inline bool isTerminator(MOp op) {
  return op == MOp::Jmp || op == MOp::Jl || op == MOp::Jne || op == MOp::Ret;
}

// What each instruction reads and writes, as named locations: registers by name,
// plus "mem" as one conservative bucket for all memory traffic, and "flags" for
// the condition codes. One honest simplification: in this IR the only flags anyone
// reads are the ones Cmp produces for a branch, so I don't model the (dead) flag
// side effects of add/sub/imul. A scheduler for real code would track flag
// liveness instead of hand-waving it; here that would only add false edges.
struct RW {
  std::set<std::string> defs, uses;
};

inline RW resourcesOf(const MInst &in) {
  RW rw;
  auto readOp = [&](const MOperand &o) {
    if (o.kind == MOperand::Kind::Reg) rw.uses.insert(o.name);
    else if (o.kind == MOperand::Kind::Mem) rw.uses.insert("mem");
  };
  auto writeOp = [&](const MOperand &o) {
    if (o.kind == MOperand::Kind::Reg) rw.defs.insert(o.name);
    else if (o.kind == MOperand::Kind::Mem) rw.defs.insert("mem");
  };
  switch (in.op) {
  case MOp::Mov:
    writeOp(in.ops[0]);
    readOp(in.ops[1]);
    break;
  case MOp::Add:
  case MOp::Sub:
  case MOp::IMul:
    readOp(in.ops[0]);   // read-modify-write destination
    writeOp(in.ops[0]);
    readOp(in.ops[1]);
    break;
  case MOp::Cmp:
    readOp(in.ops[0]);
    readOp(in.ops[1]);
    rw.defs.insert("flags");
    break;
  case MOp::Setl:
    writeOp(in.ops[0]);
    rw.uses.insert("flags");
    break;
  case MOp::Jl:
  case MOp::Jne:
    rw.uses.insert("flags");
    break;
  case MOp::Ret:
    if (!in.ops.empty()) readOp(in.ops[0]);
    break;
  case MOp::Jmp:
    break;
  }
  return rw;
}

struct DepDAG {
  int n = 0;
  std::vector<std::vector<int>> succ;
  std::vector<int> indeg;
  std::vector<int> cp;  // critical-path length from each node down to a sink
};

inline bool conflicts(const RW &a, const RW &b) {
  auto hits = [](const std::set<std::string> &x, const std::set<std::string> &y) {
    for (const std::string &s : x)
      if (y.count(s)) return true;
    return false;
  };
  return hits(a.defs, b.uses) ||  // RAW
         hits(a.uses, b.defs) ||  // WAR
         hits(a.defs, b.defs);    // WAW
}

inline DepDAG buildDAG(const std::vector<MInst> &body) {
  int n = static_cast<int>(body.size());
  std::vector<RW> rw(n);
  for (int i = 0; i < n; ++i) rw[i] = resourcesOf(body[i]);

  DepDAG d;
  d.n = n;
  d.succ.assign(n, {});
  d.indeg.assign(n, 0);
  for (int i = 0; i < n; ++i)
    for (int j = i + 1; j < n; ++j)
      if (conflicts(rw[i], rw[j])) {
        d.succ[i].push_back(j);
        d.indeg[j]++;
      }

  // Critical path: every edge points forward, so one backward sweep is enough.
  d.cp.assign(n, 0);
  for (int i = n - 1; i >= 0; --i) {
    int best = 0;
    for (int j : d.succ[i]) best = std::max(best, d.cp[j]);
    d.cp[i] = latency(body[i].op) + best;
  }
  return d;
}

// List scheduling: keep a set of "ready" instructions (all predecessors already
// placed) and, each step, emit the ready one with the longest critical path --
// the one whose chain of dependents is longest, so starting it now hides the most
// latency. Ties go to the lower original index, which keeps the output stable.
// The result is a topological order, so it computes the same thing as the input.
inline std::vector<int> listSchedule(const DepDAG &d) {
  std::vector<int> indeg = d.indeg;
  std::vector<bool> done(d.n, false);
  std::vector<int> order;
  order.reserve(d.n);
  for (int placed = 0; placed < d.n; ++placed) {
    int pick = -1;
    for (int v = 0; v < d.n; ++v) {
      if (done[v] || indeg[v] != 0) continue;
      if (pick < 0 || d.cp[v] > d.cp[pick] ||
          (d.cp[v] == d.cp[pick] && v < pick))
        pick = v;
    }
    // An acyclic graph always leaves a ready node, so pick >= 0 here.
    done[pick] = true;
    order.push_back(pick);
    for (int s : d.succ[pick]) --indeg[s];
  }
  return order;
}

inline size_t firstTerminator(const MBlock &b) {
  size_t t = 0;
  while (t < b.insts.size() && !isTerminator(b.insts[t].op)) ++t;
  return t;
}

// The plan for one block, kept around so a caller can inspect it before it's
// applied. I schedule the body -- everything before the terminator -- and leave
// the branch or ret (and their fixed relative order) untouched at the end.
struct BlockSchedule {
  std::vector<MInst> body;  // the block's instructions before the terminator
  size_t tailAt = 0;        // index of the first terminator in the original block
  DepDAG dag;
  std::vector<int> order;   // new position -> original body index
};

inline BlockSchedule planSchedule(const MBlock &b) {
  BlockSchedule s;
  s.tailAt = firstTerminator(b);
  s.body.assign(b.insts.begin(), b.insts.begin() + s.tailAt);
  s.dag = buildDAG(s.body);
  s.order = listSchedule(s.dag);
  return s;
}

inline void applySchedule(MBlock &b, const BlockSchedule &s) {
  std::vector<MInst> out;
  out.reserve(b.insts.size());
  for (int idx : s.order) out.push_back(s.body[idx]);
  for (size_t i = s.tailAt; i < b.insts.size(); ++i) out.push_back(b.insts[i]);
  b.insts = std::move(out);
}

inline void scheduleFunction(MFunction &mf) {
  for (MBlock &b : mf.blocks) applySchedule(b, planSchedule(b));
}

// --- peephole (new) ------------------------------------------------------
// A sliding window over the final stream that rewrites tiny local patterns the
// earlier stages don't bother to avoid. Two flavors show up here: identities that
// delete an instruction outright, and cheap strength reductions that swap a slow
// op for a fast one. The self-moves are exactly the ones chapter 7 pointed at --
// a copy whose source and destination were colored the same register -- so this
// is where coalescing-by-cleanup finally happens.

inline bool isReg(const MOperand &o) { return o.kind == MOperand::Kind::Reg; }
inline bool isImmVal(const MOperand &o, int64_t v) {
  return o.kind == MOperand::Kind::Imm && o.imm == v;
}

// Returns how many instructions were removed or rewritten.
inline int peepholeBlock(MBlock &b) {
  int changed = 0;
  std::vector<MInst> out;
  out.reserve(b.insts.size());
  for (MInst in : b.insts) {
    // mov r, r  -- a self-move, drop it.
    if (in.op == MOp::Mov && isReg(in.ops[0]) && isReg(in.ops[1]) &&
        in.ops[0].name == in.ops[1].name) {
      ++changed;
      continue;
    }
    // add r, 0 / sub r, 0  -- identity, drop it.
    if ((in.op == MOp::Add || in.op == MOp::Sub) && isImmVal(in.ops[1], 0)) {
      ++changed;
      continue;
    }
    // imul r, 1  -- identity, drop it.
    if (in.op == MOp::IMul && isImmVal(in.ops[1], 1)) {
      ++changed;
      continue;
    }
    // imul r, 2  ->  add r, r  -- doubling is a cheaper self-add.
    if (in.op == MOp::IMul && isImmVal(in.ops[1], 2)) {
      in.op = MOp::Add;
      in.ops[1] = in.ops[0];  // add r, r
      ++changed;
    }
    out.push_back(std::move(in));
  }
  b.insts = std::move(out);
  return changed;
}

inline int peepholeFunction(MFunction &mf) {
  int total = 0;
  for (MBlock &b : mf.blocks) total += peepholeBlock(b);
  return total;
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

inline void printMInst(std::ostream &os, const MInst &i) {
  os << "  " << mopName(i.op);
  for (size_t k = 0; k < i.ops.size(); ++k)
    os << (k ? ", " : " ") << renderMOperand(i.ops[k]);
  os << '\n';
}

inline void printMFunction(std::ostream &os, const MFunction &mf) {
  os << mf.name << ":\n";
  for (const MBlock &b : mf.blocks) {
    os << b.label << ":\n";
    for (const MInst &i : b.insts) printMInst(os, i);
  }
}

inline void printCriticalPaths(std::ostream &os, const std::vector<MInst> &body,
                               const DepDAG &d) {
  os << "critical-path priority per instruction (higher = schedule earlier):\n";
  for (int i = 0; i < d.n; ++i) {
    os << "  [cp " << d.cp[i] << "] " << mopName(body[i].op);
    for (size_t k = 0; k < body[i].ops.size(); ++k)
      os << (k ? ", " : " ") << renderMOperand(body[i].ops[k]);
    os << '\n';
  }
}

} // namespace bc
