// SSA construction. Chapter 1 gave us a three-address IR where every temporary
// was defined exactly once. That was a convenient lie: real input has variables
// that get reassigned. This file takes a function whose blocks reassign named
// variables and rewrites it into SSA form, inserting phi nodes where control
// paths merge.
//
// The pipeline is the classic Cytron et al. route:
//   preds -> dominator tree -> dominance frontiers -> phi placement -> rename.
//
// Header-only, same as chapter 1. We don't reuse chapter 1's Instruction type
// because the whole point here is to start from code that is *not* yet SSA, so
// the data model is a little different: blocks hold variable assignments.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <ostream>
#include <set>
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

// Copy is "v = w", an identity move. Phi is the merge node SSA needs.
enum class Op { Add, Sub, Mul, ICmpEq, ICmpLt, Copy, Phi, Br, CondBr, Ret };

inline const char *opName(Op op) {
  switch (op) {
  case Op::Add:    return "add";
  case Op::Sub:    return "sub";
  case Op::Mul:    return "mul";
  case Op::ICmpEq: return "icmp eq";
  case Op::ICmpLt: return "icmp lt";
  case Op::Copy:   return "copy";
  case Op::Phi:    return "phi";
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

// An operand. Before renaming, a Var operand points at the variable it reads;
// renaming fills in `text` with the concrete SSA name it resolves to. Const and
// Arg operands set `text` up front since they never change.
struct Use {
  enum class Kind { Const, Arg, Var };
  Kind kind = Kind::Const;
  int64_t imm = 0;          // Const
  Variable *var = nullptr;  // Var
  std::string text;         // resolved display string, e.g. "%m.2"
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

// A straight-line assignment: dst = op(uses). After renaming, `ssaName` holds
// the unique name this definition gets.
struct Assign {
  Variable *dst = nullptr;
  Op op = Op::Add;
  std::vector<Use> uses;
  std::string ssaName;
};

// A phi at the top of a block. `incoming[j]` is the value flowing in from the
// j-th predecessor (parallel to the block's preds vector).
struct Phi {
  Variable *var = nullptr;
  Type type = Type::I64;
  std::string ssaName;
  std::vector<std::string> incoming;
};

struct BasicBlock {
  std::string label;

  std::vector<Phi> phis;     // filled by phi insertion
  std::vector<Assign> code;  // the straight-line body

  // terminator
  Op term = Op::Ret;
  Use cond;    // CondBr
  Use retVal;  // Ret

  std::vector<BasicBlock *> succs;
  std::vector<BasicBlock *> preds;  // computed

  // dominator-tree info, computed below
  BasicBlock *idom = nullptr;
  std::vector<BasicBlock *> domChildren;
  std::set<BasicBlock *> df;  // dominance frontier
  int postorder = -1;

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

// --- step 1: predecessors -------------------------------------------------
// Successors live on the terminator; predecessors we have to invert.
inline void computePreds(Function &fn) {
  for (auto &b : fn.blocks) b->preds.clear();
  for (auto &b : fn.blocks)
    for (BasicBlock *s : b->succs) s->preds.push_back(b.get());
}

// --- step 2: dominator tree ----------------------------------------------
// Cooper, Harvey & Kennedy, "A Simple, Fast Dominance Algorithm". Number the
// blocks in postorder, then iterate idom[] to a fixpoint. intersect() walks two
// nodes up their idom chains until they meet; the node with the smaller
// postorder number (further from entry) is the one that climbs.
inline void numberPostorder(BasicBlock *b, std::set<BasicBlock *> &seen,
                            std::vector<BasicBlock *> &order) {
  seen.insert(b);
  for (BasicBlock *s : b->succs)
    if (!seen.count(s)) numberPostorder(s, seen, order);
  b->postorder = static_cast<int>(order.size());
  order.push_back(b);
}

inline BasicBlock *intersect(BasicBlock *a, BasicBlock *b) {
  while (a != b) {
    while (a->postorder < b->postorder) a = a->idom;
    while (b->postorder < a->postorder) b = b->idom;
  }
  return a;
}

inline void computeDominators(Function &fn) {
  BasicBlock *entry = fn.entry();
  std::vector<BasicBlock *> order;
  std::set<BasicBlock *> seen;
  numberPostorder(entry, seen, order);

  // reverse postorder, entry first
  std::vector<BasicBlock *> rpo(order.rbegin(), order.rend());

  for (auto &b : fn.blocks) {
    b->idom = nullptr;
    b->domChildren.clear();
  }
  entry->idom = entry;  // sentinel: entry dominates itself

  bool changed = true;
  while (changed) {
    changed = false;
    for (BasicBlock *b : rpo) {
      if (b == entry) continue;
      BasicBlock *newIdom = nullptr;
      for (BasicBlock *p : b->preds) {
        if (p->idom == nullptr) continue;  // not processed yet
        newIdom = newIdom ? intersect(p, newIdom) : p;
      }
      if (newIdom && b->idom != newIdom) {
        b->idom = newIdom;
        changed = true;
      }
    }
  }

  // build the tree from the idom pointers
  for (BasicBlock *b : rpo)
    if (b != entry) b->idom->domChildren.push_back(b);
}

// --- step 3: dominance frontiers -----------------------------------------
// DF[b] is the set of blocks where b stops dominating: the merge points just
// past b's reach. For every join (a block with >= 2 preds), walk up from each
// pred to the join's idom, marking the join as on each runner's frontier.
inline void computeDominanceFrontiers(Function &fn) {
  for (auto &b : fn.blocks) b->df.clear();
  for (auto &bp : fn.blocks) {
    BasicBlock *b = bp.get();
    if (b->preds.size() < 2) continue;
    for (BasicBlock *p : b->preds) {
      BasicBlock *runner = p;
      while (runner != b->idom) {
        runner->df.insert(b);
        runner = runner->idom;
      }
    }
  }
}

// --- step 4: phi placement -----------------------------------------------
// A variable needs a phi at every block in the iterated dominance frontier of
// its definition sites. Placing a phi is itself a definition, so it can push
// the frontier further; the worklist handles that.
inline std::vector<Variable *> varsOf(Function &fn) {
  std::vector<Variable *> out;
  for (auto &v : fn.vars) out.push_back(v.get());
  return out;
}

inline void insertPhis(Function &fn) {
  for (Variable *v : varsOf(fn)) {
    std::vector<BasicBlock *> defsites;
    std::set<BasicBlock *> isDefsite;
    for (auto &b : fn.blocks)
      for (Assign &a : b->code)
        if (a.dst == v && isDefsite.insert(b.get()).second)
          defsites.push_back(b.get());

    std::vector<BasicBlock *> worklist = defsites;
    std::set<BasicBlock *> onWorklist(defsites.begin(), defsites.end());
    std::set<BasicBlock *> hasPhi;

    while (!worklist.empty()) {
      BasicBlock *n = worklist.back();
      worklist.pop_back();
      for (BasicBlock *y : n->df) {
        if (hasPhi.count(y)) continue;
        Phi phi;
        phi.var = v;
        phi.type = v->type;
        phi.incoming.resize(y->preds.size());
        y->phis.push_back(std::move(phi));
        hasPhi.insert(y);
        if (onWorklist.insert(y).second) worklist.push_back(y);
      }
    }
  }
}

// --- step 5: rename ------------------------------------------------------
// Walk the dominator tree. Keep a stack per variable of its current SSA name;
// the top of the stack is the definition that reaches here. Reads resolve to
// the top, writes push a fresh name, and at each edge we drop the current name
// into the successor's phi slot for this predecessor.
struct Renamer {
  std::map<Variable *, std::vector<std::string>> stack;
  std::map<Variable *, int> counter;

  std::string fresh(Variable *v) {
    return v->name + "." + std::to_string(counter[v]++);
  }
  std::string top(Variable *v) {
    auto &s = stack[v];
    return s.empty() ? "undef" : s.back();
  }
  void resolve(Use &u) {
    if (u.kind == Use::Kind::Var) u.text = "%" + top(u.var);
  }

  void run(BasicBlock *b) {
    std::vector<Variable *> pushed;

    for (Phi &phi : b->phis) {
      phi.ssaName = fresh(phi.var);
      stack[phi.var].push_back(phi.ssaName);
      pushed.push_back(phi.var);
    }
    for (Assign &a : b->code) {
      for (Use &u : a.uses) resolve(u);
      a.ssaName = fresh(a.dst);
      stack[a.dst].push_back(a.ssaName);
      pushed.push_back(a.dst);
    }
    if (b->term == Op::CondBr) resolve(b->cond);
    if (b->term == Op::Ret) resolve(b->retVal);

    // fill each successor's phi operand for the edge coming from b
    for (BasicBlock *s : b->succs) {
      size_t j = 0;
      while (j < s->preds.size() && s->preds[j] != b) ++j;
      for (Phi &phi : s->phis) phi.incoming[j] = "%" + top(phi.var);
    }

    for (BasicBlock *c : b->domChildren) run(c);

    for (Variable *v : pushed) stack[v].pop_back();
  }
};

inline void renameVariables(Function &fn) {
  Renamer r;
  r.run(fn.entry());
}

inline void toSSA(Function &fn) {
  computePreds(fn);
  computeDominators(fn);
  computeDominanceFrontiers(fn);
  insertPhis(fn);
  renameVariables(fn);
}

// --- printing ------------------------------------------------------------
inline std::string renderPre(const Use &u) {
  switch (u.kind) {
  case Use::Kind::Const: return std::to_string(u.imm);
  case Use::Kind::Arg:   return u.text;             // "%a"
  case Use::Kind::Var:   return "%" + u.var->name;  // "%m"
  }
  return "?";
}

// Print the pre-SSA function: variables still reassigned, no phis.
inline void printPre(std::ostream &os, const Function &fn) {
  os << "fn " << fn.name << '(';
  for (size_t i = 0; i < fn.args.size(); ++i) {
    if (i) os << ", ";
    os << typeName(fn.args[i].second) << " %" << fn.args[i].first;
  }
  os << ") -> " << typeName(fn.retType) << " {\n";
  for (auto &bp : fn.blocks) {
    BasicBlock *b = bp.get();
    os << b->label << ":\n";
    for (Assign &a : b->code) {
      os << "  %" << a.dst->name << " = " << opName(a.op) << ' ';
      for (size_t i = 0; i < a.uses.size(); ++i) {
        if (i) os << ", ";
        os << renderPre(a.uses[i]);
      }
      os << '\n';
    }
    switch (b->term) {
    case Op::Br:
      os << "  br " << b->succs[0]->label << '\n';
      break;
    case Op::CondBr:
      os << "  condbr " << renderPre(b->cond) << ", " << b->succs[0]->label
         << ", " << b->succs[1]->label << '\n';
      break;
    default:
      os << "  ret " << renderPre(b->retVal) << '\n';
      break;
    }
  }
  os << "}\n";
}

// Print the SSA function: phis at the top of merge blocks, every name unique.
inline void printSSA(std::ostream &os, const Function &fn) {
  os << "fn " << fn.name << '(';
  for (size_t i = 0; i < fn.args.size(); ++i) {
    if (i) os << ", ";
    os << typeName(fn.args[i].second) << " %" << fn.args[i].first;
  }
  os << ") -> " << typeName(fn.retType) << " {\n";
  for (auto &bp : fn.blocks) {
    BasicBlock *b = bp.get();
    os << b->label << ":\n";
    for (Phi &phi : b->phis) {
      os << "  %" << phi.ssaName << " = phi " << typeName(phi.type) << ' ';
      for (size_t j = 0; j < phi.incoming.size(); ++j) {
        if (j) os << ", ";
        os << "[" << phi.incoming[j] << ", " << b->preds[j]->label << "]";
      }
      os << '\n';
    }
    for (Assign &a : b->code) {
      os << "  %" << a.ssaName << " = " << opName(a.op) << ' ';
      for (size_t i = 0; i < a.uses.size(); ++i) {
        if (i) os << ", ";
        os << a.uses[i].text;
      }
      os << '\n';
    }
    switch (b->term) {
    case Op::Br:
      os << "  br " << b->succs[0]->label << '\n';
      break;
    case Op::CondBr:
      os << "  condbr " << b->cond.text << ", " << b->succs[0]->label << ", "
         << b->succs[1]->label << '\n';
      break;
    default:
      os << "  ret " << b->retVal.text << '\n';
      break;
    }
  }
  os << "}\n";
}

} // namespace bc
