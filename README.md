# linscan

A reproduction of Poletto & Sarkar, *Linear Scan Register Allocation*
(ACM TOPLAS 21(5), Sept. 1999, pp. 895-913).

The goal is to test the paper's claims on a small compiler backend of our own:
a plain (non-SSA) IR, a register VM, and three allocators (usage-count,
linear scan, simple graph coloring).

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

## Experiments

```
cmake -S . -B build && cmake --build build
tools/run_experiments.sh build        # writes results/*.csv (about two minutes)
python3 tools/analyze.py              # writes results/summary.md and the figure analogues

```
```
```
## Pathological programs

```
python3 tools/gen_pathological.py a --n 512 > bench/generated/path_a_512.ir
python3 tools/gen_pathological.py b --k 10 --m 8 > bench/generated/path_b_10_8.ir
```

Each file starts with a `# expect: N` line giving the value the program prints.
