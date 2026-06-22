// A tiny three-address IR. This is the thing the backend works on: a function is
// a list of basic blocks, a block is a list of instructions, and most
// instructions produce a value that later ones can use.
//
// It's header-only so a chapter is just this file plus main.cpp. Not in SSA form
// yet (that's chapter 2).
#pragma once

#include <cstdint>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

namespace bc {

enum class Type { Void, I1, I64 };

inline const char *typeName(Type t) {
  switch (t) {
  case Type::Void: return "void";
  case Type::I1:   return "i1";
  case Type::I64:  return "i64";
  }
  return "?";
}

enum class Op { Add, Sub, Mul, ICmpEq, ICmpLt, Br, CondBr, Ret };

inline const char *opName(Op op) {
  switch (op) {
  case Op::Add:    return "add";
  case Op::Sub:    return "sub";
  case Op::Mul:    return "mul";
  case Op::ICmpEq: return "icmp eq";
  case Op::ICmpLt: return "icmp lt";
  case Op::Br:     return "br";
  case Op::CondBr: return "condbr";
  case Op::Ret:    return "ret";
  }
  return "?";
}

inline bool isTerminator(Op op) {
  return op == Op::Br || op == Op::CondBr || op == Op::Ret;
}

struct BasicBlock;

// Anything an instruction can refer to: a constant, an argument, or another
// instruction's result. One base class means an operand is just a Value*.
struct Value {
  enum class Kind { Constant, Argument, Instruction };
  Kind kind;
  Type type;
  std::string name; // for printing
  Value(Kind k, Type t, std::string n) : kind(k), type(t), name(std::move(n)) {}
  virtual ~Value() = default;
};

struct Constant : Value {
  int64_t imm;
  Constant(int64_t v, Type t)
      : Value(Kind::Constant, t, std::to_string(v)), imm(v) {}
};

struct Argument : Value {
  Argument(Type t, std::string n)
      : Value(Kind::Argument, t, std::move(n)) {}
};

struct Instruction : Value {
  Op op;
  std::vector<Value *> operands;
  std::vector<BasicBlock *> succs; // successor blocks for br / condbr
  Instruction(Op o, Type t, std::string n)
      : Value(Kind::Instruction, t, std::move(n)), op(o) {}
};

struct BasicBlock {
  std::string label;
  std::vector<Instruction *> insts;

  Instruction *terminator() const {
    if (insts.empty()) return nullptr;
    Instruction *last = insts.back();
    return isTerminator(last->op) ? last : nullptr;
  }
};

// Owns every value and block. The pointer vectors above are just views into
// these pools, so operand references stay valid as we build.
struct Function {
  std::string name;
  Type retType = Type::Void;
  std::vector<Argument *> args;
  std::vector<BasicBlock *> blocks;
  std::vector<std::unique_ptr<Value>> valuePool;
  std::vector<std::unique_ptr<BasicBlock>> blockPool;

  Argument *addArg(Type t, std::string n) {
    auto a = std::make_unique<Argument>(t, std::move(n));
    Argument *p = a.get();
    valuePool.push_back(std::move(a));
    args.push_back(p);
    return p;
  }

  BasicBlock *addBlock(std::string label) {
    auto b = std::make_unique<BasicBlock>();
    b->label = std::move(label);
    BasicBlock *p = b.get();
    blockPool.push_back(std::move(b));
    blocks.push_back(p);
    return p;
  }
};

// Builds instructions into the current block and names temporaries for you.
class IRBuilder {
public:
  explicit IRBuilder(Function &f) : fn(f) {}
  void setBlock(BasicBlock *b) { bb = b; }

  Constant *constant(int64_t v, Type t = Type::I64) {
    auto c = std::make_unique<Constant>(v, t);
    Constant *p = c.get();
    fn.valuePool.push_back(std::move(c));
    return p;
  }

  Value *add(Value *a, Value *b)    { return make(Op::Add, Type::I64, {a, b}); }
  Value *sub(Value *a, Value *b)    { return make(Op::Sub, Type::I64, {a, b}); }
  Value *mul(Value *a, Value *b)    { return make(Op::Mul, Type::I64, {a, b}); }
  Value *icmpEq(Value *a, Value *b) { return make(Op::ICmpEq, Type::I1, {a, b}); }
  Value *icmpLt(Value *a, Value *b) { return make(Op::ICmpLt, Type::I1, {a, b}); }

  void br(BasicBlock *dest) {
    make(Op::Br, Type::Void, {})->succs = {dest};
  }
  void condBr(Value *cond, BasicBlock *ifT, BasicBlock *ifF) {
    make(Op::CondBr, Type::Void, {cond})->succs = {ifT, ifF};
  }
  void ret(Value *v) { make(Op::Ret, Type::Void, {v}); }

private:
  Instruction *make(Op op, Type t, std::vector<Value *> ops) {
    std::string nm = (t == Type::Void) ? std::string() : "t" + std::to_string(tmp++);
    auto in = std::make_unique<Instruction>(op, t, std::move(nm));
    Instruction *p = in.get();
    p->operands = std::move(ops);
    fn.valuePool.push_back(std::move(in));
    bb->insts.push_back(p);
    return p;
  }

  Function &fn;
  BasicBlock *bb = nullptr;
  unsigned tmp = 0;
};

inline void printRef(std::ostream &os, const Value *v) {
  if (v->kind == Value::Kind::Constant)
    os << static_cast<const Constant *>(v)->imm;
  else
    os << '%' << v->name;
}

inline void print(std::ostream &os, const Function &fn) {
  os << "fn " << fn.name << '(';
  for (size_t i = 0; i < fn.args.size(); ++i) {
    if (i) os << ", ";
    os << typeName(fn.args[i]->type) << " %" << fn.args[i]->name;
  }
  os << ") -> " << typeName(fn.retType) << " {\n";
  for (BasicBlock *bb : fn.blocks) {
    os << bb->label << ":\n";
    for (Instruction *in : bb->insts) {
      os << "  ";
      switch (in->op) {
      case Op::Add: case Op::Sub: case Op::Mul:
      case Op::ICmpEq: case Op::ICmpLt:
        os << '%' << in->name << " = " << opName(in->op) << ' ';
        printRef(os, in->operands[0]);
        os << ", ";
        printRef(os, in->operands[1]);
        break;
      case Op::Br:
        os << "br " << in->succs[0]->label;
        break;
      case Op::CondBr:
        os << "condbr ";
        printRef(os, in->operands[0]);
        os << ", " << in->succs[0]->label << ", " << in->succs[1]->label;
        break;
      case Op::Ret:
        os << "ret ";
        printRef(os, in->operands[0]);
        break;
      }
      os << '\n';
    }
  }
  os << "}\n";
}

} // namespace bc
