#!/usr/bin/env python3
"""Generate the pathological programs of Poletto & Sarkar (1999), Sec. 5.2.3, Fig. 4.

  a --n N          N variables that are all simultaneously live (Fig. 4a)
  b --k K --m M    K * M variables in a sliding window with at most M live at once (Fig. 4b)
  b-acc --k K --m M  the same window plus one accumulator that stays live throughout (see below)

Output is the text IR from DESIGN.md. The first lines are comments, including
"# expect: <values>", the numbers the program prints, in order.

Fig. 4(b) is described only as "k staggered sets of live intervals in which no more than m
overlap". Our reading, `b`, is pure intervals: each variable is defined, lives while m - 1
others are defined after it, and is consumed by a `print`, so every variable has exactly two
mentions and no interval spans the program.

`b-acc` is the first reading we tried: the variables are summed into one accumulator that is
live from start to end. That adds one interval that overlaps all the others, which Fig. 4(b)
does not show. It matters: an allocator that spills "the interval that ends last" evicts the
accumulator, which is used on every step, so the two readings give different code quality.
Both are kept so the difference is visible; `b` is the one that matches the figure.
"""
import argparse
import sys


def gen_a(n):
    if n < 1:
        sys.exit("n must be >= 1")
    acc = 0  # accumulate into v0 so exactly n variables are live at the peak
    out = [
        f"# pathological type (a): {n} simultaneously live variables",
        f"# expect: {n * (n + 1) // 2}",
        f"func path_a_{n}",
        "block entry:",
    ]
    out += [f"  v{i} = const {i + 1}" for i in range(n)]
    out += [f"  v{acc} = add v{acc} v{i}" for i in range(1, n)]
    out += [f"  print v{acc}", "  ret"]
    return "\n".join(out) + "\n"


def gen_b(k, m):
    """Pure staggered intervals: at most m live at once, no variable lives longer than m steps."""
    if k < 1 or m < 1:
        sys.exit("need k >= 1 and m >= 1")
    total = k * m
    out = [
        f"# pathological type (b): k={k} sets, at most m={m} live at once ({total} variables, pure intervals)",
        "# expect: " + " ".join(str(i + 1) for i in range(total)),
        f"func path_b_{k}_{m}",
        "block entry:",
    ]
    out += [f"  v{i} = const {i + 1}" for i in range(m)]
    for i in range(m, total):
        out.append(f"  print v{i - m}")       # the oldest variable dies here ...
        out.append(f"  v{i} = const {i + 1}")  # ... and a new one is born, so m are live
    out += [f"  print v{i}" for i in range(total - m, total)]
    out += ["  ret"]
    return "\n".join(out) + "\n"


def gen_b_acc(k, m):
    if k < 1 or m < 2:
        sys.exit("need k >= 1 and m >= 2")
    window = m - 1          # plus the accumulator, so at most m are live at once
    total = k * m
    acc = total
    out = [
        f"# pathological type (b-acc): k={k} sets, at most m={m} live at once ({total} window variables + a long-lived accumulator)",
        f"# expect: {total * (total + 1) // 2}",
        f"func path_bacc_{k}_{m}",
        "block entry:",
        f"  v{acc} = const 0",
    ]
    out += [f"  v{i} = const {i + 1}" for i in range(window)]
    for i in range(window, total):
        out.append(f"  v{acc} = add v{acc} v{i - window}")
        out.append(f"  v{i} = const {i + 1}")
    out += [f"  v{acc} = add v{acc} v{i}" for i in range(total - window, total)]
    out += [f"  print v{acc}", "  ret"]
    return "\n".join(out) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="kind", required=True)
    pa = sub.add_parser("a")
    pa.add_argument("--n", type=int, required=True)
    for kind in ("b", "b-acc"):
        pb = sub.add_parser(kind)
        pb.add_argument("--k", type=int, required=True)
        pb.add_argument("--m", type=int, required=True)
    args = ap.parse_args()
    if args.kind == "a":
        sys.stdout.write(gen_a(args.n))
    elif args.kind == "b":
        sys.stdout.write(gen_b(args.k, args.m))
    else:
        sys.stdout.write(gen_b_acc(args.k, args.m))


if __name__ == "__main__":
    main()
