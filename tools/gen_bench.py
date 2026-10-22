#!/usr/bin/env python3
"""Generate the benchmark kernels in bench/*.ir.

The kernels stand in for the paper's SPEC / icode programs, which we cannot reproduce.
They are written with a small builder, run through a tiny IR interpreter that lives in THIS
file (so the expected output does not come from the C++ interpreter under test), and written
out with a "# expect: v1 v2 ..." header line. tests/test_bench.cpp checks that the C++
reference interpreter prints exactly those values, and that every allocator reproduces them.

Where a kernel has a known answer (fib, primes, sortedness) the generator also asserts it, so
the kernel is checked against its intent and not only against itself.

  python3 tools/gen_bench.py            # writes bench/*.ir
"""
import os
import sys

MASK = (1 << 64) - 1


def wrap(x):
    x &= MASK
    return x - (1 << 64) if x >> 63 else x


class Prog:
    """Builds a function in the text IR. Variables are numbered v0, v1, ... in creation order."""

    def __init__(self, name, note):
        self.name, self.note = name, note
        self.blocks = []  # [name, [instruction tuples]]
        self.nv = 0
        self.nlabel = 0
        self.begin("entry")
        self.one = self.const(1)

    def v(self):
        self.nv += 1
        return self.nv - 1

    def label(self, prefix):
        self.nlabel += 1
        return f"{prefix}{self.nlabel}"

    def begin(self, name):
        self.blocks.append([name, []])

    def emit(self, *ins):
        self.blocks[-1][1].append(ins)

    # --- instructions; each returns the destination variable ---
    def const(self, c, dst=None):
        dst = self.v() if dst is None else dst
        self.emit("const", dst, c)
        return dst

    def op(self, name, a, b, dst=None):
        dst = self.v() if dst is None else dst
        self.emit(name, dst, a, b)
        return dst

    def mov(self, a, dst=None):
        dst = self.v() if dst is None else dst
        self.emit("mov", dst, a)
        return dst

    def load(self, addr, dst=None):
        dst = self.v() if dst is None else dst
        self.emit("load", dst, addr)
        return dst

    def store(self, addr, val):
        self.emit("store", addr, val)

    def print(self, a):
        self.emit("print", a)

    def counted_loop(self, i, limit, body, step=None):
        """for (; i < limit; i += step) body()   (i must already hold the start value; step defaults to 1)."""
        head, bd, out = self.label("head"), self.label("body"), self.label("out")
        self.emit("jmp", head)
        self.begin(head)
        t = self.op("lt", i, limit)
        self.emit("br", t, bd, out)
        self.begin(bd)
        body()
        self.op("add", i, self.one if step is None else step, dst=i)
        self.emit("jmp", head)
        self.begin(out)

    def finish(self):
        self.emit("ret")

    # --- output ---
    def text(self, expect):
        out = [f"# {self.note}", "# expect: " + " ".join(str(x) for x in expect), f"func {self.name}"]
        for name, ins in self.blocks:
            out.append(f"block {name}:")
            for t in ins:
                k = t[0]
                if k == "const":
                    out.append(f"  v{t[1]} = const {t[2]}")
                elif k in ("add", "sub", "mul", "lt"):
                    out.append(f"  v{t[1]} = {k} v{t[2]} v{t[3]}")
                elif k in ("mov", "load"):
                    out.append(f"  v{t[1]} = {k} v{t[2]}")
                elif k == "store":
                    out.append(f"  store v{t[1]} v{t[2]}")
                elif k == "print":
                    out.append(f"  print v{t[1]}")
                elif k == "jmp":
                    out.append(f"  jmp {t[1]}")
                elif k == "br":
                    out.append(f"  br v{t[1]} {t[2]} {t[3]}")
                elif k == "ret":
                    out.append("  ret")
                else:
                    raise ValueError(k)
        return "\n".join(out) + "\n"

    def run(self, max_steps=50_000_000):
        """An independent interpreter for the IR (the oracle for the expected output)."""
        index = {name: n for n, (name, _) in enumerate(self.blocks)}
        reg, mem, out = [0] * self.nv, {}, []
        b, i, steps = 0, 0, 0
        while True:
            steps += 1
            if steps > max_steps:
                raise RuntimeError(f"{self.name}: step limit")
            t = self.blocks[b][1][i]
            k = t[0]
            i += 1
            if k == "const":
                reg[t[1]] = wrap(t[2])
            elif k == "add":
                reg[t[1]] = wrap(reg[t[2]] + reg[t[3]])
            elif k == "sub":
                reg[t[1]] = wrap(reg[t[2]] - reg[t[3]])
            elif k == "mul":
                reg[t[1]] = wrap(reg[t[2]] * reg[t[3]])
            elif k == "lt":
                reg[t[1]] = 1 if reg[t[2]] < reg[t[3]] else 0
            elif k == "mov":
                reg[t[1]] = reg[t[2]]
            elif k == "load":
                reg[t[1]] = mem.get(reg[t[2]], 0)
            elif k == "store":
                mem[reg[t[1]]] = reg[t[2]]
            elif k == "print":
                out.append(reg[t[1]])
            elif k == "jmp":
                b, i = index[t[1]], 0
            elif k == "br":
                b, i = index[t[2] if reg[t[1]] != 0 else t[3]], 0
            elif k == "ret":
                return out


# ----------------------------------------------------------------------------- kernels

def fib(n=30):
    p = Prog("fib", f"iterative Fibonacci, fib({n}); few variables, one hot loop")
    a, b, i, lim = p.const(0), p.const(1), p.const(0), p.const(n)
    def body():
        c = p.op("add", a, b)
        p.mov(b, dst=a)
        p.mov(c, dst=b)
    p.counted_loop(i, lim, body)
    p.print(a)
    p.finish()
    return p, lambda out: out == [832040]


def matmul(n=6):
    p = Prog("matmul", f"{n}x{n} matrix multiply through memory, checksum of C; three nested loops")
    N = p.const(n)
    base_a, base_b, base_c = p.const(0), p.const(1000), p.const(2000)
    # A[i][j] = i + j + 1, B[i][j] = i - j + 2
    i = p.const(0)
    def init_i():
        j = p.const(0)
        def init_j():
            idx = p.op("add", p.op("mul", i, N), j)
            p.store(p.op("add", base_a, idx), p.op("add", p.op("add", i, j), p.one))
            p.store(p.op("add", base_b, idx), p.op("add", p.op("sub", i, j), p.const(2)))
        p.counted_loop(j, N, init_j)
    p.counted_loop(i, N, init_i)
    # C = A * B
    i2 = p.const(0)
    def row():
        j2 = p.const(0)
        def col():
            k = p.const(0)
            acc = p.const(0)
            def inner():
                a = p.load(p.op("add", base_a, p.op("add", p.op("mul", i2, N), k)))
                b = p.load(p.op("add", base_b, p.op("add", p.op("mul", k, N), j2)))
                p.op("add", acc, p.op("mul", a, b), dst=acc)
            p.counted_loop(k, N, inner)
            p.store(p.op("add", base_c, p.op("add", p.op("mul", i2, N), j2)), acc)
        p.counted_loop(j2, N, col)
    p.counted_loop(i2, N, row)
    # checksum
    q = p.const(0)
    total = p.v()
    p.const(0, dst=total)
    NN = p.op("mul", N, N)
    def sumc():
        p.op("add", total, p.load(p.op("add", base_c, q)), dst=total)
    p.counted_loop(q, NN, sumc)
    p.print(total)
    p.finish()
    A = [[i + j + 1 for j in range(n)] for i in range(n)]
    B = [[i - j + 2 for j in range(n)] for i in range(n)]
    want = sum(sum(A[i][k] * B[k][j] for k in range(n)) for i in range(n) for j in range(n))
    return p, lambda out: out == [want]


def sieve(limit=200):
    p = Prog("sieve", f"count primes below {limit} with a sieve of flags in memory; nested loops")
    lim = p.const(limit)
    count = p.const(0)
    pr = p.const(2)
    def outer():
        flag = p.load(pr)
        is_prime = p.op("lt", flag, p.one)  # flag == 0
        yes, no = p.label("prime"), p.label("composite")
        p.emit("br", is_prime, yes, no)
        p.begin(yes)
        p.op("add", count, p.one, dst=count)
        m = p.op("add", pr, pr)
        def mark():
            p.store(m, p.one)
        p.counted_loop(m, lim, mark, step=pr)
        p.emit("jmp", no)
        p.begin(no)
    p.counted_loop(pr, lim, outer)
    p.print(count)
    p.finish()
    return p, lambda out: out == [46]  # primes below 200


def bubble(n=24):
    p = Prog("bubble", f"fill {n} words from an LCG, bubble sort them, print first, last and a checksum")
    N = p.const(n)
    mult, inc = p.const(6364136223846793005), p.const(1442695040888963407)
    x = p.const(12345)
    base = p.const(0)
    i = p.const(0)
    def fill():
        p.op("add", p.op("mul", x, mult), inc, dst=x)
        p.store(p.op("add", base, i), x)
    p.counted_loop(i, N, fill)
    pass_ = p.const(0)
    Nm1 = p.op("sub", N, p.one)
    def sort_pass():
        j = p.const(0)
        def step():
            a_addr = p.op("add", base, j)
            b_addr = p.op("add", a_addr, p.one)
            a, b = p.load(a_addr), p.load(b_addr)
            gt = p.op("lt", b, a)
            swap, keep = p.label("swap"), p.label("keep")
            p.emit("br", gt, swap, keep)
            p.begin(swap)
            p.store(a_addr, b)
            p.store(b_addr, a)
            p.emit("jmp", keep)
            p.begin(keep)
        p.counted_loop(j, Nm1, step)
    p.counted_loop(pass_, N, sort_pass)
    first = p.load(base)
    last = p.load(p.op("add", base, Nm1))
    chk, q = p.const(0), p.const(0)
    def checksum():
        p.op("add", p.op("mul", chk, mult), p.load(p.op("add", base, q)), dst=chk)
    p.counted_loop(q, N, checksum)
    p.print(first)
    p.print(last)
    p.print(chk)
    p.finish()

    def ok(out):
        vals, v = [], 12345
        for _ in range(n):
            v = wrap(v * 6364136223846793005 + 1442695040888963407)
            vals.append(v)
        vals.sort()
        c = 0
        for e in vals:
            c = wrap(c * 6364136223846793005 + e)
        return out == [vals[0], vals[-1], c]
    return p, ok


def fir(taps, samples=60):
    p = Prog(f"fir{taps}", f"{taps}-tap FIR filter: {taps} coefficients and a {taps}-deep window all live across the loop")
    coef = [p.const(3 * k + 1) for k in range(taps)]
    win = [p.const(0) for _ in range(taps)]
    mult, inc = p.const(1103515245), p.const(12345)
    seed = p.const(42)
    total = p.const(0)
    n = p.const(samples)
    i = p.const(0)
    def body():
        p.op("add", p.op("mul", seed, mult), inc, dst=seed)  # next input sample
        for k in range(taps - 1, 0, -1):
            p.mov(win[k - 1], dst=win[k])
        p.mov(seed, dst=win[0])
        acc = p.op("mul", win[0], coef[0])
        for k in range(1, taps):
            acc = p.op("add", acc, p.op("mul", win[k], coef[k]))
        p.op("add", total, acc, dst=total)
    p.counted_loop(i, n, body)
    p.print(total)
    p.finish()
    return p, None


def mix(words=8, rounds=200):
    p = Prog("mix8", f"{words} state words mixed for {rounds} rounds; every state word is loop-carried and hot")
    s = [p.const(7 * k + 1) for k in range(words)]
    mult = p.const(2862933555777941757)
    i, n = p.const(0), p.const(rounds)
    def body():
        for k in range(words):
            p.op("add", p.op("mul", s[k], mult), s[(k + 1) % words], dst=s[k])
    p.counted_loop(i, n, body)
    t = p.const(0)
    for k in range(words):
        p.op("add", t, s[k], dst=t)
    p.print(t)
    p.finish()
    return p, None


def hot_long(hot=6, cold=8, iters=100):
    p = Prog("hot_long", f"{hot} hot variables live from the start to the end, but {cold} short-lived cold temporaries appear before the hot loop")
    H = [p.const(k + 2) for k in range(hot)]
    # cold phase: many temporaries live together, long after the hot ones started
    C = [p.const(10 * k + 5) for k in range(cold)]
    cs = p.const(0)
    for c in C:
        p.op("add", cs, c, dst=cs)
    p.print(cs)
    # hot phase
    acc, i, n = p.const(0), p.const(0), p.const(iters)
    def body():
        for k in range(hot):
            p.op("add", acc, p.op("mul", H[k], H[(k + 1) % hot]), dst=acc)
    p.counted_loop(i, n, body)
    fin = p.const(0)
    for h in H:
        p.op("add", fin, h, dst=fin)  # the hot variables are also read once more at the very end
    p.print(acc)
    p.print(fin)
    p.finish()
    return p, None


def cold_long(cold=6, hot=6, iters=100):
    p = Prog("cold_long", f"{cold} cold variables live from the start to the end, a hot loop of {hot} short-lived temporaries in between")
    C = [p.const(k + 3) for k in range(cold)]
    acc, i, n = p.const(0), p.const(0), p.const(iters)
    def body():
        t = [p.op("mul", i, p.const(k + 1)) for k in range(hot)]
        for x in t:
            p.op("add", acc, x, dst=acc)
    p.counted_loop(i, n, body)
    cs = p.const(0)
    for c in C:
        p.op("add", cs, c, dst=cs)
    p.print(acc)
    p.print(cs)
    p.finish()
    return p, None


def branchy(diamonds=150):
    p = Prog("branchy", f"{diamonds} if/else diamonds in a row ({3 * diamonds + 1} basic blocks) over 5 variables: many blocks, few variables")
    x, y, z = p.const(3), p.const(5), p.const(0)
    two = p.const(2)
    seven = p.const(7)
    for _ in range(diamonds):
        t = p.op("lt", x, y)
        a, b, c = p.label("then"), p.label("else"), p.label("join")
        p.emit("br", t, a, b)
        p.begin(a)
        p.op("add", x, two, dst=x)
        p.emit("jmp", c)
        p.begin(b)
        p.op("add", y, seven, dst=y)
        p.emit("jmp", c)
        p.begin(c)
        p.op("add", z, x, dst=z)
    p.print(z)
    p.print(y)
    p.finish()
    return p, None


KERNELS = [
    fib(), matmul(), sieve(), bubble(), fir(6), fir(12), fir(20), mix(), hot_long(), cold_long(), branchy(),
]


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    out_dir = os.path.join(here, "..", "bench")
    os.makedirs(out_dir, exist_ok=True)
    for prog, check in KERNELS:
        want = prog.run()
        if check is not None and not check(want):
            sys.exit(f"{prog.name}: the kernel does not compute what it is meant to: printed {want}")
        path = os.path.join(out_dir, prog.name + ".ir")
        with open(path, "w") as f:
            f.write(prog.text(want))
        nblocks = len(prog.blocks)
        print(f"{prog.name:10s} {prog.nv:4d} variables {nblocks:4d} blocks   prints {want[:4]}")


if __name__ == "__main__":
    main()
