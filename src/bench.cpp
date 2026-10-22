// linscan_bench: the measurement driver.
//
//
// For every program, numbering, register count and allocator it
//   1. times the compile-time phases separately, over `trials` independent trials,
//   2. allocates once more (untimed), checks the result with the static validator, runs it on
//      the VM and compares what it prints with the reference run and with the program's own
//      "# expect:" line, and records the executed-instruction and cycle counts.
// A wrong result stops the run with a non-zero exit: no number is reported for an allocation
// that does not work. Rows go to a CSV, one per trial.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "alloc.h"
#include "cfg.h"
#include "interference.h"
#include "intervals.h"
#include "ir.h"
#include "liveness.h"
#include "parser.h"
#include "validate.h"
#include "vm.h"

using namespace ls;

namespace {
volatile size_t g_sink = 0;  // keeps the timed work from being optimised away

using Clock = std::chrono::steady_clock;

double elapsed_ns(Clock::time_point a, Clock::time_point b) {
  return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
}

// Times `work` (which returns something cheap derived from its result, to be consumed) over
// `trials` trials. One trial runs it `reps` times back to back and reports the mean per run, so a
// phase that takes a few microseconds is still measured against a loop well above the clock's
// resolution (DESIGN.md). `reps` is chosen once, from one calibration run after a warm-up run,
// so that a trial lasts at least min_ms; slow phases run once per trial.
std::vector<double> time_phase(const std::function<size_t()>& work, int trials, double min_ms) {
  g_sink += work();  // warm-up: first touch of the code and the data
  const auto t0 = Clock::now();
  g_sink += work();
  const double one = std::max(1.0, elapsed_ns(t0, Clock::now()));
  const double target = min_ms * 1e6;
  const long reps = std::clamp(static_cast<long>(std::ceil(target / one)), 1L, 2000000L);

  std::vector<double> per_run;
  for (int t = 0; t < trials; ++t) {
    const auto a = Clock::now();
    size_t sink = 0;
    for (long r = 0; r < reps; ++r) sink += work();
    const auto b = Clock::now();
    g_sink += sink;
    per_run.push_back(elapsed_ns(a, b) / static_cast<double>(reps));
  }
  return per_run;
}

// ---------------------------------------------------------------------------- allocators

struct AllocSpec {
  std::string name;
  bool needs_interference;
  SharingRule sharing;
  std::function<Allocation(const AllocProblem&)> run;
};

const std::vector<AllocSpec>& all_allocators() {
  static const std::vector<AllocSpec> v = {
      {"usage", false, SharingRule::Intervals, [](const AllocProblem& p) { return allocate_usage_count(p); }},
      {"linear", false, SharingRule::Intervals, [](const AllocProblem& p) { return allocate_linear_scan(p); }},
      {"linear-weight", false, SharingRule::Intervals,
       [](const AllocProblem& p) { return allocate_linear_scan_with(p, LinearSpillChoice::Weight); }},
      {"color", true, SharingRule::Interference, [](const AllocProblem& p) { return allocate_graph_coloring(p); }},
      {"color-ratio", true, SharingRule::Interference,
       [](const AllocProblem& p) { return allocate_graph_coloring_with(p, SpillChoice::LowestWeightPerDegree); }},
  };
  return v;
}

// ---------------------------------------------------------------------------- inputs

struct Program {
  std::string name;
  Function f;
  bool has_expect = false;
  std::vector<long> expect;
  int instrs = 0;
};

Program load_program(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot open " + path);
  std::stringstream buf;
  buf << in.rdbuf();
  const std::string text = buf.str();

  Program p;
  size_t slash = path.find_last_of('/');
  p.name = path.substr(slash == std::string::npos ? 0 : slash + 1);
  if (p.name.size() > 3 && p.name.compare(p.name.size() - 3, 3, ".ir") == 0) p.name.resize(p.name.size() - 3);

  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    if (line.rfind("# expect:", 0) != 0) continue;
    p.has_expect = true;
    std::istringstream nums(line.substr(9));
    long x;
    while (nums >> x) p.expect.push_back(x);
    break;
  }
  std::istringstream src(text);
  p.f = parse_function(src);
  for (const Block& b : p.f.blocks) p.instrs += static_cast<int>(b.instrs.size());
  return p;
}

std::vector<std::string> split(const std::string& s) {
  std::vector<std::string> out;
  std::stringstream ss(s);
  std::string item;
  while (std::getline(ss, item, ',')) out.push_back(item);
  return out;
}

struct Options {
  std::string experiment = "adhoc";
  std::vector<int> Rs = {8};
  std::vector<std::string> allocs = {"usage", "linear", "color"};
  std::vector<std::string> numberings = {"depth"};
  int trials = 10;
  double min_ms = 10.0;
  bool run_vm = true;
  std::string out;
  std::vector<std::string> files;
};

[[noreturn]] void usage(const std::string& why) {
  std::cerr << "linscan_bench: " << why << "\n"
            << "usage: linscan_bench [--experiment NAME] [--R 4,8,16] [--alloc usage,linear,linear-weight,color,color-ratio]\n"
            << "                     [--numbering depth,layout] [--trials N] [--min-ms MS] [--no-vm] [--out FILE.csv] file.ir...\n";
  std::exit(2);
}

Options parse_args(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) usage("missing value after " + a);
      return argv[++i];
    };
    if (a == "--experiment") o.experiment = next();
    else if (a == "--R") { o.Rs.clear(); for (const auto& x : split(next())) o.Rs.push_back(std::stoi(x)); }
    else if (a == "--alloc") o.allocs = split(next());
    else if (a == "--numbering") o.numberings = split(next());
    else if (a == "--trials") o.trials = std::stoi(next());
    else if (a == "--min-ms") o.min_ms = std::stod(next());
    else if (a == "--no-vm") o.run_vm = false;
    else if (a == "--out") o.out = next();
    else if (!a.empty() && a[0] == '-') usage("unknown option " + a);
    else o.files.push_back(a);
  }
  if (o.files.empty()) usage("no input programs");
  if (o.trials < 1) usage("--trials must be at least 1");
  for (const auto& n : o.numberings) if (n != "depth" && n != "layout") usage("numbering must be depth or layout");
  for (const auto& n : o.allocs) {
    bool known = false;
    for (const auto& s : all_allocators()) known = known || s.name == n;
    if (!known) usage("unknown allocator " + n);
  }
  return o;
}

}  // namespace

int main(int argc, char** argv) {
  const Options opt = parse_args(argc, argv);

  std::ofstream file;
  if (!opt.out.empty()) {
    file.open(opt.out);
    if (!file) usage("cannot write " + opt.out);
  }
  std::ostream& csv = opt.out.empty() ? std::cout : file;
  csv << "experiment,program,allocator,numbering,R,trial,t_liveness_ns,t_intervals_ns,t_setup_ns,t_alloc_ns,"
         "spills_static,executed,cycles,spill_loads,spill_stores,vars,instrs,blocks\n";

  int failures = 0;
  for (const std::string& path : opt.files) {
    Program prog;
    try {
      prog = load_program(path);
    } catch (const std::exception& e) {
      std::cerr << "FAIL " << path << ": " << e.what() << "\n";
      ++failures;
      continue;
    }
    const Function& f = prog.f;

    // Ground truth, once per program.
    RunStats ref_stats;
    std::vector<long> want;
    if (opt.run_vm) {
      want = run_reference(f, &ref_stats);
      if (prog.has_expect && want != prog.expect) {
        std::cerr << "FAIL " << prog.name << ": the reference run disagrees with the program's own '# expect:' line\n";
        ++failures;
        continue;
      }
    }

    bool want_interference = false;
    for (const auto& a : opt.allocs)
      for (const auto& s : all_allocators()) want_interference = want_interference || (s.name == a && s.needs_interference);

    // ---- phases that do not depend on R or on the allocator ----
    const Liveness live = compute_liveness(f);
    const std::vector<double> t_live = time_phase([&] { return compute_liveness(f).live_in.size(); }, opt.trials, opt.min_ms);

    std::vector<std::vector<int>> interference;
    std::vector<double> t_interf(opt.trials, 0.0);
    if (want_interference) {
      interference = build_interference(f, live);
      t_interf = time_phase([&] { return build_interference(f, live).size(); }, opt.trials, opt.min_ms);
    }

    for (const std::string& nname : opt.numberings) {
      const bool depth = nname == "depth";
      auto order = [&] { return depth ? depth_first_order(f) : layout_order(f); };

      // Setup for linear scan and usage-count: number the instructions and build the intervals.
      const std::vector<Interval> intervals = build_intervals(f, live, order());
      const std::vector<double> t_int =
          time_phase([&] { return build_intervals(f, live, order()).size(); }, opt.trials, opt.min_ms);

      for (int R : opt.Rs) {
        AllocProblem p;
        p.num_vars = f.num_vars;
        p.R = R;
        p.intervals = intervals;
        p.interference = interference;  // empty unless a coloring allocator was requested

        for (const std::string& aname : opt.allocs) {
          const AllocSpec* spec = nullptr;
          for (const auto& s : all_allocators()) if (s.name == aname) spec = &s;

          // Allocation proper, on the finished problem.
          const std::vector<double> t_alloc =
              time_phase([&] { return spec->run(p).reg.size(); }, opt.trials, opt.min_ms);

          // Untimed: the allocation must be correct before any number about it means anything.
          const Allocation a = spec->run(p);
          RunStats st;
          if (opt.run_vm) {
            std::string why;
            if (!validate_allocation(p, a, &why, spec->sharing)) {
              std::cerr << "FAIL " << prog.name << " " << aname << " R=" << R << ": validator: " << why << "\n";
              ++failures;
              continue;
            }
            VmConfig cfg;
            cfg.R = R;
            std::vector<long> got;
            try {
              got = run_allocated(f, a, cfg, &st);
            } catch (const std::exception& e) {
              std::cerr << "FAIL " << prog.name << " " << aname << " R=" << R << ": run failed: " << e.what() << "\n";
              ++failures;
              continue;
            }
            if (got != want) {
              std::cerr << "FAIL " << prog.name << " " << aname << " R=" << R << ": output differs from the reference run\n";
              ++failures;
              continue;
            }
          }

          for (int t = 0; t < opt.trials; ++t) {
            const double setup = spec->needs_interference ? t_interf[t] : t_int[t];
            char row[512];
            std::snprintf(row, sizeof row, "%s,%s,%s,%s,%d,%d,%.1f,%.1f,%.1f,%.1f,%d,%ld,%ld,%ld,%ld,%d,%d,%zu\n",
                          opt.experiment.c_str(), prog.name.c_str(), aname.c_str(), nname.c_str(), R, t, t_live[t],
                          t_int[t], setup, t_alloc[t], a.num_spills, st.executed, st.cycles, st.spill_loads,
                          st.spill_stores, f.num_vars, prog.instrs, f.blocks.size());
            csv << row;
          }
        }
      }
    }
    std::cerr << "done " << prog.name << " (" << f.num_vars << " vars, " << prog.instrs << " instrs, " << f.blocks.size() << " blocks)\n";
  }
  if (failures) std::cerr << failures << " failure(s)\n";
  return failures ? 1 : 0;
}
