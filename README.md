# linscan

A reproduction of Poletto & Sarkar, *Linear Scan Register Allocation*
(ACM TOPLAS 21(5), Sept. 1999, pp. 895-913).

The goal is to test the paper's claims on a small compiler backend of our own:
a plain (non-SSA) IR, a register VM, and three allocators (usage-count,
linear scan, simple graph coloring). See `CLAIMS.md` for exactly what is being
tested, `DESIGN.md` for the decisions that are frozen, and `RESULTS.md` for the
write-up.

## Quick start

```
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Development build with sanitizers (use this while writing code):

```
cmake -S . -B build-dev -DCMAKE_BUILD_TYPE=Debug -DSANITIZE=ON
cmake --build build-dev
ctest --test-dir build-dev --output-on-failure
```

Benchmarks and timings always come from the optimized `build/` directory
(`-O2`), never from `build-dev/`.

## State

Everything in the build order is implemented and tested (steps 1 to 7). `ctest` runs five tests:
`validator`, `linear_scan`, `harness`, `allocators` (all three allocators and their variants, through the
differential harness and against hand-built cases, oracles and mutants), and `bench_programs` (every kernel in
`bench/` against an independently computed expected output).

## Experiments

```
cmake -S . -B build && cmake --build build
tools/run_experiments.sh build        # writes results/*.csv (about two minutes)
python3 tools/analyze.py              # writes results/summary.md and the figure analogues
```

`tools/gen_bench.py` regenerates the kernels in `bench/`; `tools/gen_pathological.py` the pathological programs.
`linscan_bench` is the driver; run it with no arguments for its options. Use a quiet machine: the timings are
wall-clock, and `results/summary.md` reports the run-to-run noise it saw.

## Build order

Tick these off as you go.

- [ ] 1. Parser for the text IR, then the reference interpreter
- [ ] 2. DFS numbering, iterative liveness, live intervals (check by hand on `tests/sum_loop.ir`)
- [ ] 3. VM with spill support, plus the differential harness
- [ ] 4. Usage-count allocator
- [ ] 5. Linear scan (Fig. 1 of the paper) -> **milestone 1**
- [ ] 6. Simple graph coloring with reference-count spill choice
- [ ] 7. Pathological experiments (Figs. 5 and 6), then the rest of `CLAIMS.md` -> **milestone 2**

## Layout

```
src/     ir, parser, cfg, liveness, intervals, vm, validate, alloc_*, main
tests/   test_core.cpp, plus small .ir programs with .expected outputs
bench/   benchmark programs (generated pathological ones go in bench/generated/)
tools/   gen_pathological.py
DESIGN.md   CLAIMS.md   RESULTS.md
```

## Pathological programs

```
python3 tools/gen_pathological.py a --n 512 > bench/generated/path_a_512.ir
python3 tools/gen_pathological.py b --k 10 --m 8 > bench/generated/path_b_10_8.ir
```

Each file starts with a `# expect: N` line giving the value the program prints.
