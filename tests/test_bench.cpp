// Every benchmark kernel in bench/ must (1) print what its generator's independent interpreter says it
// prints (the "# expect:" line), and (2) be reproduced exactly by every allocator, at several register
// counts and under both numberings, through the differential harness. The bench driver relies on both.
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "alloc.h"
#include "harness.h"
#include "parser.h"
#include "vm.h"

using namespace ls;

static int g_bad = 0;
static void check(bool ok, const std::string& what) {
  if (!ok) ++g_bad;
  std::cout << (ok ? "ok    " : "FAIL  ") << what << "\n";
}

int main() {
  const std::vector<std::string> names = {"fib", "matmul", "sieve", "bubble", "fir6", "fir12", "fir20", "mix8", "hot_long", "cold_long", "branchy"};
  struct Alloc { std::string name; Allocator fn; SharingRule sharing; };
  const std::vector<Alloc> allocs = {
      {"usage-count", allocate_usage_count, SharingRule::Intervals},
      {"linear scan", allocate_linear_scan, SharingRule::Intervals},
      {"linear scan (weight)", [](const AllocProblem& p) { return allocate_linear_scan_with(p, LinearSpillChoice::Weight); }, SharingRule::Intervals},
      {"graph coloring", allocate_graph_coloring, SharingRule::Interference},
      {"graph coloring (weight/degree)", [](const AllocProblem& p) { return allocate_graph_coloring_with(p, SpillChoice::LowestWeightPerDegree); }, SharingRule::Interference},
  };
  const int Rs[] = {1, 2, 3, 4, 8, 16, 32};

  for (const std::string& name : names) {
    const std::string path = std::string(LINSCAN_SOURCE_DIR) + "/bench/" + name + ".ir";
    std::ifstream in(path);
    if (!in) { check(false, name + ": cannot open " + path + " (run tools/gen_bench.py)"); continue; }
    std::stringstream buf;
    buf << in.rdbuf();
    std::vector<long> expect;
    bool has = false;
    std::istringstream lines(buf.str());
    for (std::string line; std::getline(lines, line);)
      if (line.rfind("# expect:", 0) == 0) {
        has = true;
        std::istringstream nums(line.substr(9));
        for (long x; nums >> x;) expect.push_back(x);
        break;
      }
    std::istringstream src(buf.str());
    const Function f = parse_function(src);

    check(has && !expect.empty() && run_reference(f) == expect, name + ": the C++ reference interpreter prints what the generator's independent interpreter says");

    int combos = 0, failures = 0, spilled = 0;
    std::string first;
    for (const Alloc& a : allocs)
      for (int R : Rs)
        for (Numbering nu : {Numbering::DepthFirst, Numbering::Layout}) {
          CheckOptions opt;
          opt.numbering = nu;
          opt.sharing = a.sharing;
          const CheckResult r = check_allocation(f, R, a.fn, opt);
          ++combos;
          if (r.num_spills > 0) ++spilled;
          if (!r.ok && failures++ == 0) first = a.name + " R=" + std::to_string(R) + ": " + r.why;
        }
    check(failures == 0, name + ": " + std::to_string(combos) + " allocator x R x numbering combinations pass the harness (" + std::to_string(spilled) + " with spills)" + (failures ? "  FIRST: " + first : ""));
  }
  std::cout << (g_bad ? "FAILED" : "all passed") << "\n";
  return g_bad ? 1 : 0;
}
