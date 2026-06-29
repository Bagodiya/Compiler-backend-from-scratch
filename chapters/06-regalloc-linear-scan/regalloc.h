// Register allocation, part 1: linear scan.
//
// Selection (chapter 5) handed us machine instructions, but they still run on
// *virtual* registers -- as many as the IR wanted. Real x86-64 has sixteen
// general-purpose registers, and a couple of those are spoken for (the stack and
// frame pointers). So before this can be real code, every virtual register has
// to be mapped onto a physical one, and when there aren't enough to go around,
// some values have to live in memory instead. That mapping is register
// allocation. This chapter does the fast, simple version: linear scan.
//
// I carry the IR core, the machine layer, and the selector forward from chapter
// 5 unchanged, then add live-interval computation and the scan itself at the
// bottom. Graph coloring -- the slower, better allocator -- is chapter 7.
#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

namespace bc {

// --- the IR (carried over from chapter 5) --------------------------------

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

// --- the machine layer (carried over from chapter 5) ---------------------

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

// --- the selector (carried over from chapter 5) --------------------------

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

// --- live intervals (new) ------------------------------------------------
// A virtual register is "live" from the first instruction that mentions it to
// the last one that does. Squash that down to a pair of instruction numbers and
// you have its live interval. Two intervals that overlap can't share a physical
// register; two that don't, can. The whole allocator is built on that one fact.

struct Interval {
  int vreg = 0;
  int start = 0;
  int end = 0;  // inclusive
};

// Flatten the function into one numbered instruction stream and record, per
// virtual register, the first and last position it appears at.
//
// This is the linear-scan simplification, and it's worth being honest about it:
// we trust the linear order of instructions to stand in for real liveness. The
// moment there are branches, that's only approximate -- a value used in one
// successor but defined before a different branch can get an interval that's too
// long or too short. The faithful version reuses the liveness dataflow from
// chapter 3 to number a linearized CFG. Our example here is a single
// straight-line block, so the linear order *is* exact and we sidestep the issue.
inline std::vector<Interval> computeIntervals(const MFunction &mf,
                                              std::map<int, std::string> &names) {
  std::map<int, Interval> iv;
  int pos = 0;
  for (const MBlock &b : mf.blocks) {
    for (const MInst &in : b.insts) {
      for (const MOperand &o : in.ops) {
        if (o.kind != MOperand::Kind::Reg) continue;
        auto it = iv.find(o.reg);
        if (it == iv.end()) {
          iv[o.reg] = Interval{o.reg, pos, pos};
          names[o.reg] = o.name;
        } else {
          it->second.end = pos;  // a later mention; start is already fixed
        }
      }
      ++pos;
    }
  }
  std::vector<Interval> out;
  out.reserve(iv.size());
  for (auto &kv : iv) out.push_back(kv.second);
  // Linear scan walks intervals in order of increasing start point.
  std::sort(out.begin(), out.end(), [](const Interval &x, const Interval &y) {
    if (x.start != y.start) return x.start < y.start;
    return x.vreg < y.vreg;
  });
  return out;
}

// --- linear scan (new) ---------------------------------------------------
// The decision for one virtual register: a physical register, or a stack slot.

struct Alloc {
  bool spilled = false;
  std::string reg;  // physical register, when !spilled
  int slot = -1;    // stack slot index, when spilled
};

struct AllocResult {
  std::vector<Interval> intervals;
  std::map<int, std::string> names;  // vreg id -> name
  std::map<int, Alloc> alloc;        // vreg id -> decision
  int numSpills = 0;
};

// Keep the active list sorted by increasing end point, so the longest-living
// interval -- the spill candidate -- is always at the back.
inline void insertActive(std::vector<Interval> &active, const Interval &i) {
  auto at = std::lower_bound(
      active.begin(), active.end(), i,
      [](const Interval &a, const Interval &b) { return a.end < b.end; });
  active.insert(at, i);
}

// The algorithm itself. Walk intervals left to right. Before placing each one,
// retire any active interval that has already ended and hand its register back.
// If a register is free, take it; if not, spill the interval whose live range
// reaches furthest -- which might be the new one.
inline AllocResult linearScan(const MFunction &mf,
                              const std::vector<std::string> &pool) {
  AllocResult R;
  R.intervals = computeIntervals(mf, R.names);

  // free works as a stack; seeding it reversed makes pool[0] come out first,
  // which only matters for keeping the printout tidy.
  std::vector<std::string> free(pool.rbegin(), pool.rend());
  std::vector<Interval> active;  // sorted by end point

  for (const Interval &i : R.intervals) {
    // Expire intervals that end strictly before i begins. Note the "strictly":
    // if one ends exactly where another starts we treat them as overlapping.
    // That's conservative -- a read and a write at the same instruction could
    // in principle share -- but it keeps the rule simple and never wrong.
    for (size_t k = 0; k < active.size();) {
      if (active[k].end < i.start) {
        free.push_back(R.alloc[active[k].vreg].reg);
        active.erase(active.begin() + k);
      } else {
        ++k;
      }
    }

    if (active.size() == pool.size()) {
      // No free register. The back of `active` lives longest.
      int evVreg = active.back().vreg;
      int evEnd = active.back().end;
      if (evEnd > i.end) {
        // The active hog outlives i: evict it, give its register to i.
        R.alloc[i.vreg] = Alloc{false, R.alloc[evVreg].reg, -1};
        R.alloc[evVreg] = Alloc{true, "", R.numSpills++};
        active.pop_back();
        insertActive(active, i);
      } else {
        // i itself reaches furthest, so i is the cheapest thing to spill.
        R.alloc[i.vreg] = Alloc{true, "", R.numSpills++};
      }
    } else {
      std::string reg = free.back();
      free.pop_back();
      R.alloc[i.vreg] = Alloc{false, reg, -1};
      insertActive(active, i);
    }
  }
  return R;
}

// Rewrite every virtual-register operand in place to its allocated location: a
// physical register name, or a memory reference for a spilled value.
//
// One honest caveat: this naive rewrite can put a value in memory where x86
// wouldn't allow it -- imul, for instance, won't take a memory destination. Our
// example only spills a value that's touched by mov and add (both fine with a
// memory operand), so the result is legal here. In general a spill has to be
// reloaded into a scratch register around each use; that lowering belongs with
// the rest of code emission in chapter 9.
inline void applyAllocation(MFunction &mf, const AllocResult &R) {
  for (MBlock &b : mf.blocks)
    for (MInst &in : b.insts)
      for (MOperand &o : in.ops) {
        if (o.kind != MOperand::Kind::Reg) continue;
        const Alloc &a = R.alloc.at(o.reg);
        if (a.spilled) {
          o.kind = MOperand::Kind::Mem;
          o.name = "rbp-" + std::to_string((a.slot + 1) * 8);
        } else {
          o.name = a.reg;
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

inline void printIntervals(std::ostream &os, const AllocResult &R) {
  os << "live intervals (by instruction number):\n";
  for (const Interval &i : R.intervals)
    os << "  %" << R.names.at(i.vreg) << "  [" << i.start << ", " << i.end
       << "]\n";
}

inline void printAllocation(std::ostream &os, const AllocResult &R) {
  os << "allocation:\n";
  for (const Interval &i : R.intervals) {
    const Alloc &a = R.alloc.at(i.vreg);
    os << "  %" << R.names.at(i.vreg) << " -> ";
    if (a.spilled)
      os << "[rbp-" << (a.slot + 1) * 8 << "]  (spilled)\n";
    else
      os << "%" << a.reg << "\n";
  }
}

} // namespace bc
