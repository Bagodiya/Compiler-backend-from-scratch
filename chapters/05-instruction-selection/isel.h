// Instruction selection: the first step that stops being target-independent.
// Everything up to here -- the IR, SSA, dataflow, the optimizations -- talked
// about "add" and "icmp lt" as if a machine would just do them. It won't. A real
// CPU has its own instructions with their own quirks, and selection is the pass
// that picks machine instructions to cover the IR. We target x86-64.
//
// I carry the IR core and its printer forward from chapter 4 unchanged so this
// file stands on its own, then drop the dataflow/optimization machinery -- none
// of it is needed to pick instructions. Everything new lives under "the machine
// layer" and after.
//
// One thing to keep straight: the machine instructions we emit still use *virtual
// registers*, one per IR variable, as many as we like. Cutting those down to the
// handful the hardware actually has is register allocation, chapters 6 and 7.
// And we don't yet know the calling convention, so `ret` just carries its value
// for now; pinning it to the ABI return register is chapter 9.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

namespace bc {

// --- the IR (carried over from chapter 4) --------------------------------

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

// --- the machine layer (new) ---------------------------------------------
// A deliberately tiny slice of x86-64. Enough opcodes for arithmetic, a compare,
// and the jumps. Operands are still virtual registers, immediates, or branch
// labels. Two things to notice in the opcode set:
//
//   - There is no three-address `add`. x86 arithmetic is two-address: `add %a,
//     %b` means %a = %a + %b, the destination is also a source. Selecting a
//     three-address IR `add` therefore costs a `mov` plus an `add`.
//   - There is no boolean type. A compare sets the flags register as a side
//     effect, and you read those flags either with a conditional jump (`jl`) or
//     by materializing a 0/1 into a register (`setl`).

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
  enum class Kind { Reg, Imm, Label };
  Kind kind = Kind::Imm;
  int reg = 0;          // Reg: virtual register id
  std::string name;     // Reg: its name; Label: the target label
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

// --- the selector (new) --------------------------------------------------
// Our IR is already linearized into three-address form, so we don't tile an
// expression tree the way a classic maximal-munch selector does. We walk the
// instructions and expand each into machine ops, with two tiles that span more
// than one IR node where it pays off:
//
//   - fold an immediate operand into add/sub/cmp instead of loading it first
//     (x86 takes a 32-bit immediate in those instructions);
//   - fuse a compare feeding directly into a branch (`icmp lt` then `condbr`)
//     into a `cmp` plus a conditional jump, skipping the boolean register.
//
// Everything else is straight macro expansion.

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

  // Some operand slots can't hold an immediate (the left side of a `cmp`, the
  // source of an `imul`). When one shows up there, materialize it into a fresh
  // register with a `mov` and use that instead.
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
      // Two-address: copy the first operand into the destination, then operate
      // in place with the second.
      MOperand dst = regForVar(a.dst);
      emit(mb, MOp::Mov, {dst, operandOf(a.uses[0])});
      MOperand rhs = operandOf(a.uses[1]);
      MOp m = a.op == Op::Add ? MOp::Add : a.op == Op::Sub ? MOp::Sub : MOp::IMul;
      if (m == MOp::IMul) rhs = intoReg(rhs, mb);  // imul wants a register source
      emit(mb, m, {dst, rhs});
      break;
    }
    case Op::ICmpLt: {
      // The unfused case: we actually need the boolean in a register. `cmp` sets
      // the flags, `setl` turns "was less than" into a 0/1.
      MOperand lhs = intoReg(operandOf(a.uses[0]), mb);
      emit(mb, MOp::Cmp, {lhs, operandOf(a.uses[1])});
      emit(mb, MOp::Setl, {regForVar(a.dst)});
      break;
    }
    default:
      break;  // branches and ret are terminators, handled in selectBlock
    }
  }

  void selectBlock(BasicBlock *b, MBlock &mb,
                   const std::map<Variable *, int> &reads) {
    size_t n = b->code.size();

    // Can we fuse the last compare into the branch? Only if the branch tests a
    // variable that the last instruction just computed with `icmp lt`, and
    // nothing else reads it (so we don't still owe anyone the boolean value).
    bool fuse = false;
    if (b->term == Op::CondBr && b->cond.kind == Use::Kind::Var && n > 0) {
      const Assign &last = b->code[n - 1];
      auto it = reads.find(b->cond.var);
      if (last.op == Op::ICmpLt && last.dst == b->cond.var &&
          it != reads.end() && it->second == 1)
        fuse = true;
    }

    size_t emitN = fuse ? n - 1 : n;  // hold back the compare if we'll fuse it
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
        // The boolean is already in a register; test it against zero.
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

// Count how many times each variable is *read*, across code and terminators.
// The fusion test needs this: a compare result with a single reader (its branch)
// is safe to dissolve into a conditional jump.
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
  // Give the arguments registers up front so they keep their nice names.
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

} // namespace bc
