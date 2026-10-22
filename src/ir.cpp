#include "ir.h"

#include <algorithm>
#include <sstream>

namespace ls {

const char* op_name(Op op) {
  switch (op) {
    case Op::Const: return "const";
    case Op::Mov:   return "mov";
    case Op::Add:   return "add";
    case Op::Sub:   return "sub";
    case Op::Mul:   return "mul";
    case Op::Lt:    return "lt";
    case Op::Load:  return "load";
    case Op::Store: return "store";
    case Op::Jmp:   return "jmp";
    case Op::Br:    return "br";
    case Op::Print: return "print";
    case Op::Ret:   return "ret";
  }
  return "?";
}

int def(const Instr& i) {
  switch (i.op) {
    case Op::Const: case Op::Mov: case Op::Add: case Op::Sub:
    case Op::Mul: case Op::Lt: case Op::Load:
      return i.dst;
    default:
      return -1;
  }
}

std::vector<int> uses(const Instr& i) {
  std::vector<int> u;
  switch (i.op) {
    case Op::Add: case Op::Sub: case Op::Mul: case Op::Lt: case Op::Store:
      u = {i.a, i.b};
      break;
    case Op::Mov: case Op::Load: case Op::Print: case Op::Ret: case Op::Br:
      u = {i.a};
      break;
    case Op::Const: case Op::Jmp:
      break;
  }
  u.erase(std::remove(u.begin(), u.end(), -1), u.end());
  return u;
}

std::string to_string(const Function& f) {
  std::ostringstream os;
  os << "func " << f.name << "\n";
  for (const Block& b : f.blocks) {
    os << "block " << b.name << ":\n";
    for (const Instr& i : b.instrs) {
      os << "  ";
      if (def(i) >= 0) os << "v" << i.dst << " = ";
      os << op_name(i.op);
      if (i.op == Op::Const) os << " " << i.imm;
      for (int u : uses(i)) os << " v" << u;
      if (i.op == Op::Jmp) os << " " << f.blocks.at(i.t1).name;
      if (i.op == Op::Br) os << " " << f.blocks.at(i.t1).name << " " << f.blocks.at(i.t2).name;
      os << "\n";
    }
  }
  return os.str();
}

}  // namespace ls
