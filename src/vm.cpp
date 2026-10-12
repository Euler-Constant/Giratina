#include "vm.h"

#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "common.h"

namespace ls {

namespace {

// Programs that loop forever (a bad .ir file, or a bug in an allocator that corrupts
// a loop counter) stop with an error instead of hanging.
constexpr long kMaxSteps = 200000000;

// Values wrap on overflow (DESIGN.md). Signed overflow is undefined behaviour in C++,
// so do the arithmetic unsigned and convert back.
long wrap_add(long a, long b) { return static_cast<long>(static_cast<unsigned long>(a) + static_cast<unsigned long>(b)); }
long wrap_sub(long a, long b) { return static_cast<long>(static_cast<unsigned long>(a) - static_cast<unsigned long>(b)); }
long wrap_mul(long a, long b) { return static_cast<long>(static_cast<unsigned long>(a) * static_cast<unsigned long>(b)); }

// The one interpreter loop. The reference run and the allocated run differ only in how a
// variable is read and written, so both go through here: a mismatch between their outputs
// then points at the allocation, not at a typo in one of two copies of the switch.
//
// `read(v)` and `write(v, x)` access variable v. `s` is updated for every instruction
// executed; the callbacks add any spill traffic to it themselves.
template <class Read, class Write>
std::vector<long> interpret(const Function& f, Read& read, Write& write, const VmConfig& cfg,
                            RunStats& s) {
  std::vector<long> out;
  std::unordered_map<long, long> mem;  // addressable memory; unwritten addresses read as 0

  int b = 0;      // current block
  size_t i = 0;   // index of the next instruction within it
  long steps = 0;

  while (true) {
    // .at() throws if control runs off the end of a block. The parser guarantees every
    // block ends in a terminator, so this only fires for hand-built Functions.
    const Instr& in = f.blocks.at(b).instrs.at(i);

    if (++steps > kMaxSteps) throw std::runtime_error("step limit exceeded (infinite loop?)");
    s.executed++;
    s.cycles += (in.op == Op::Load) ? cfg.cost_load : (in.op == Op::Store) ? cfg.cost_store : cfg.cost_alu;

    switch (in.op) {
      case Op::Const:
        write(in.dst, in.imm);
        ++i;
        break;
      case Op::Mov: {
        long x = read(in.a);
        write(in.dst, x);
        ++i;
        break;
      }
      case Op::Add: {
        long x = read(in.a), y = read(in.b);
        write(in.dst, wrap_add(x, y));
        ++i;
        break;
      }
      case Op::Sub: {
        long x = read(in.a), y = read(in.b);
        write(in.dst, wrap_sub(x, y));
        ++i;
        break;
      }
      case Op::Mul: {
        long x = read(in.a), y = read(in.b);
        write(in.dst, wrap_mul(x, y));
        ++i;
        break;
      }
      case Op::Lt: {
        long x = read(in.a), y = read(in.b);
        write(in.dst, x < y ? 1 : 0);
        ++i;
        break;
      }
      case Op::Load: {
        long addr = read(in.a);
        auto it = mem.find(addr);
        write(in.dst, it == mem.end() ? 0 : it->second);
        ++i;
        break;
      }
      case Op::Store: {
        long addr = read(in.a), val = read(in.b);
        mem[addr] = val;
        ++i;
        break;
      }
      case Op::Jmp:
        b = in.t1;
        i = 0;
        break;
      case Op::Br: {
        long cond = read(in.a);
        b = (cond != 0) ? in.t1 : in.t2;
        i = 0;
        break;
      }
      case Op::Print:
        out.push_back(read(in.a));
        ++i;
        break;
      case Op::Ret:
        return out;
    }
  }
}

}  // namespace

// Ground truth: every variable has its own storage, nothing is ever spilled.
std::vector<long> run_reference(const Function& f, RunStats* stats) {
  std::vector<long> vars(f.num_vars, 0);
  auto read = [&](int v) { return vars.at(v); };
  auto write = [&](int v, long x) { vars.at(v) = x; };

  RunStats s;
  VmConfig cfg;  // default cycle weights
  std::vector<long> out = interpret(f, read, write, cfg, s);
  if (stats) *stats = s;
  return out;
}

// Runs the function as allocated. A variable with a register lives in `regs`. A spilled
// variable lives in `slots`: reading it is a spill load and writing it is a spill store,
// each counted as one executed instruction (DESIGN.md). The scratch registers that would
// hold those values in a real machine are transient, so they are not modelled.
std::vector<long> run_allocated(const Function& f, const Allocation& a, const VmConfig& cfg,
                                RunStats* stats) {
  if (static_cast<int>(a.reg.size()) != f.num_vars || static_cast<int>(a.slot.size()) != f.num_vars)
    throw std::runtime_error("allocation does not match the function's variable count");

  int max_slot = -1;
  for (int sl : a.slot) max_slot = sl > max_slot ? sl : max_slot;

  std::vector<long> regs(cfg.R, 0);
  std::vector<long> slots(max_slot + 1, 0);
  RunStats s;

  // Where does variable v live? Throws if it has no home or an impossible one.
  auto home_error = [](int v, const char* why) {
    throw std::runtime_error("v" + std::to_string(v) + ": " + why);
  };

  auto read = [&](int v) -> long {
    int r = a.reg.at(v);
    if (r >= cfg.R) home_error(v, "register number is outside R");
    if (r >= 0) return regs[r];
    int sl = a.slot.at(v);
    if (sl < 0) home_error(v, "has neither a register nor a stack slot");
    s.spill_loads++;
    s.executed++;
    s.cycles += cfg.cost_load;
    return slots[sl];
  };

  auto write = [&](int v, long x) {
    int r = a.reg.at(v);
    if (r >= cfg.R) home_error(v, "register number is outside R");
    if (r >= 0) {
      regs[r] = x;
      return;
    }
    int sl = a.slot.at(v);
    if (sl < 0) home_error(v, "has neither a register nor a stack slot");
    s.spill_stores++;
    s.executed++;
    s.cycles += cfg.cost_store;
    slots[sl] = x;
  };

  std::vector<long> out = interpret(f, read, write, cfg, s);
  if (stats) *stats = s;
  return out;
}

}  // namespace ls
