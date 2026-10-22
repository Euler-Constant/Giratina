#pragma once
#include <string>
#include <vector>

namespace ls {

enum class Op { Const, Mov, Add, Sub, Mul, Lt, Load, Store, Jmp, Br, Print, Ret };

const char* op_name(Op op);

struct Instr {
  Op op = Op::Ret;
  int dst = -1;          // virtual register defined, or -1
  int a = -1, b = -1;    // virtual registers read, or -1 (Br: a is the condition)
  long imm = 0;          // Const value
  int t1 = -1, t2 = -1;  // branch targets as block indices (Jmp: t1; Br: t1 if true, t2 if false)
};

struct Block {
  std::string name;
  std::vector<Instr> instrs;  // last instruction is Jmp, Br, or Ret
};

struct Function {
  std::string name;
  std::vector<Block> blocks;  // blocks[0] is the entry
  int num_vars = 0;           // variables are v0 .. v(num_vars-1)
};

int def(const Instr& i);                // variable written, or -1
std::vector<int> uses(const Instr& i);  // variables read
std::string to_string(const Function& f);

}  // namespace ls
