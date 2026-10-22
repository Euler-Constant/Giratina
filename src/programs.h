#pragma once
// Programs shared by the tests.
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "ir.h"
#include "parser.h"

inline ls::Function parse(const std::string& src) {
  std::istringstream in(src);
  return ls::parse_function(in);
}

// Terminating random programs. Every variable is given a distinct value at the start, so a
// clobbered register or slot changes what is printed (and no variable is read before it is
// written, which the VM does not define). Control flow only goes forward, apart from one
// counted loop around the whole body when with_loop is set. At the end v0 and a random half
// of the other variables are printed: most values are observable, but some variables die
// early, which lets allocators legitimately reuse their registers and stack slots.
inline std::string random_program(std::mt19937& rng, bool with_loop) {
  const int n = 2 + rng() % 6;  // program variables v0 .. v(n-1)
  const int m = 1 + rng() % 4;  // body blocks b0 .. b(m-1)
  const int C = n, N = n + 1, ONE = n + 2, T = n + 3;  // loop counter, bound, 1, test
  std::ostringstream s;
  s << "func rnd\nblock entry:\n";
  for (int i = 0; i < n; ++i) s << "  v" << i << " = const " << (i * 7 + 3) << "\n";
  if (with_loop)
    s << "  v" << C << " = const 0\n  v" << N << " = const " << (2 + rng() % 2) << "\n  v" << ONE << " = const 1\n";
  s << "  jmp b0\n";

  auto var = [&]() { return static_cast<int>(rng() % n); };
  const std::string tail = with_loop ? "latch" : "exit";
  auto target = [&](int from) {  // a block after `from`, or the tail
    int j = from + 1 + static_cast<int>(rng() % (m - from));
    return j >= m ? tail : "b" + std::to_string(j);
  };

  for (int i = 0; i < m; ++i) {
    s << "block b" << i << ":\n";
    const int body = rng() % 5;
    for (int k = 0; k < body; ++k) {
      switch (rng() % 8) {
        case 0: s << "  v" << var() << " = add v" << var() << " v" << var() << "\n"; break;
        case 1: s << "  v" << var() << " = sub v" << var() << " v" << var() << "\n"; break;
        case 2: s << "  v" << var() << " = mul v" << var() << " v" << var() << "\n"; break;
        case 3: s << "  v" << var() << " = mov v" << var() << "\n"; break;
        case 4: s << "  v" << var() << " = const " << (rng() % 50) << "\n"; break;
        case 5: s << "  v" << var() << " = lt v" << var() << " v" << var() << "\n"; break;
        case 6: s << "  store v" << var() << " v" << var() << "\n"; break;
        default: s << "  print v" << var() << "\n"; break;
      }
    }
    if (rng() % 2) s << "  jmp " << target(i) << "\n";
    else s << "  br v" << var() << " " << target(i) << " " << target(i) << "\n";
  }
  if (with_loop) {
    s << "block latch:\n  v" << C << " = add v" << C << " v" << ONE << "\n  v" << T << " = lt v" << C
      << " v" << N << "\n  br v" << T << " b0 exit\n";
  }
  // Print only some of the variables at the end. If all of them were printed they would all
  // be live from the entry to the exit, no two intervals would ever be disjoint, and the
  // allocators would never get to legitimately share a register or a stack slot.
  s << "block exit:\n";
  for (int i = 0; i < n; ++i)
    if (i == 0 || rng() % 2) s << "  print v" << i << "\n";
  s << "  ret\n";
  return s.str();
}

inline std::string type_a(int n) {  // n variables live at the same time
  std::ostringstream s;
  s << "func a\nblock entry:\n";
  for (int i = 0; i < n; ++i) s << "  v" << i << " = const " << i + 1 << "\n";
  for (int i = 1; i < n; ++i) s << "  v0 = add v0 v" << i << "\n";
  s << "  print v0\n  ret\n";
  return s.str();
}

// The hand-written programs every correct allocator must handle, at every R and numbering.
// sum_loop is read from tests/sum_loop.ir under source_dir.
struct Named {
  std::string name;
  ls::Function f;
};
inline std::vector<Named> fixed_programs(const std::string& source_dir) {
  std::ifstream in(source_dir + "/tests/sum_loop.ir");
  if (!in) throw std::runtime_error("cannot open " + source_dir + "/tests/sum_loop.ir");
  std::vector<Named> v;
  v.push_back({"sum_loop", ls::parse_function(in)});
  v.push_back({"all ops (wrap-around, load/store)", parse(
      "func t\nblock a:\n  v0 = const 9223372036854775807\n  v1 = const 1\n  v2 = add v0 v1\n"
      "  v3 = const 100\n  store v3 v2\n  v4 = load v3\n  print v4\n  v5 = load v1\n  print v5\n"
      "  v6 = mul v0 v0\n  print v6\n  v7 = sub v1 v0\n  print v7\n  v8 = mov v1\n"
      "  v9 = lt v8 v3\n  v10 = lt v3 v8\n  print v9\n  print v10\n  ret\n")});
  v.push_back({"nested loops", parse(
      "func t\nblock entry:\n  v0 = const 0\n  v1 = const 3\n  jmp outer\n"
      "block outer:\n  v2 = lt v0 v1\n  br v2 inner exit\n"
      "block inner:\n  v0 = add v0 v1\n  v3 = lt v0 v1\n  br v3 inner outer\n"
      "block exit:\n  print v0\n  ret\n")});
  v.push_back({"diamond", parse(
      "func t\nblock entry:\n  v0 = const 4\n  v1 = const 9\n  v2 = lt v0 v1\n  br v2 l r\n"
      "block l:\n  v3 = add v0 v1\n  jmp join\nblock r:\n  v3 = sub v0 v1\n  jmp join\n"
      "block join:\n  print v3\n  print v0\n  ret\n")});
  v.push_back({"type (a) n=40", parse(type_a(40))});
  return v;
}
