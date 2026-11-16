# Max-Cut — Approximation Competition

You are given a working approximation algorithm for Max-Cut,
`maxcut_foundation.cpp`: the randomized 1/2-approximation from the lecture
(put every vertex on a random side; each edge is cut with probability 1/2)
followed by the obvious local improvement (move any vertex that has more
weight on its own side than across). Copy it to `solver.cpp` and make its
cuts **heavier**. Same input, a valid cut out, 30 seconds, one thread.

**Weights can be negative.** Four of the six scored graphs are *signed*: a
negative edge rewards keeping its two endpoints on the **same** side. The
lecture's 1/2 guarantee does not survive this — half the total weight can be
about zero — so on those graphs a random cut is worth nothing and everything
comes from the search.

This is not a speed contest. Every solver gets the same 30 s per instance.
What is measured is **how close your cut is to the optimum**: a proven upper
bound on the maximum cut divided by the weight of your cut. 1.000 would be
optimal. The foundation scores about 1.20.

**Your entire job is one C++ file.** Everything else — running, timing,
checking, scoring — is done by one Python script, `grade.py`. You hand in
`solver.cpp` and the `result.json` it writes by uploading them to the
scoreboard at **https://maxcut.ccu2026algorithm.workers.dev**
(see [What to submit](#what-to-submit)).

## How the pieces fit

![Your solver.cpp and the six graphs go into grade.py; a score table and result.json come out](slides/workflow.svg)

You only touch `solver.cpp`. One command produces the table on the right, and
the `Overall geomean ratio` line is your score. **Lower is better.**

## Quick start

```sh
make foundation                          # 1. build the baseline
scripts/download.sh                      # 2. fetch the six scored graphs (9 MB, once)
cp maxcut_foundation.cpp solver.cpp      # 3. this file is your assignment
make solver                              #    ...edit solver.cpp, rebuild...
python3 grade.py --solver ./solver --instances instances_dev.txt              # 4. quick check (1 min)
python3 grade.py --solver ./solver --instances instances.txt --json result.json   # 5. score (3 min)
```

Step 2 downloads the six scored graphs (25 MB uncompressed, too large for
git) and checks every file's SHA-256 against `checksums/`; the practice graphs
are already in the repo. Run it again after every `git pull`: it keeps the
files that still match and fetches any that are missing or outdated. Step 5
prints a table and writes `result.json`. Before you change anything,
`solver.cpp` *is* the foundation, so this is what you see (times in seconds):

```
category   instance         n        m limit   wall        cut      bound   ratio     gap  note
----------------------------------------------------------------------------------------------------------
POSITIVE   tri400tail  160000   480000    30    0.6    9888193   10819248  1.0942   9.42%  ok
POSITIVE   dlat400     160000   480000    30    0.1    9926591   10995384  1.1077  10.77%  ok
SIGNED     tri300s      90000   270000    30    0.0    2266456    2823442  1.2458  24.58%  ok
SIGNED     tri200s4     40000   120000    30    0.0     857703    1049436  1.2235  22.35%  ok
SIGNED     tri200g      40000   120000    30    0.0     816389    1039228  1.2730  27.30%  ok
SIGNED     vbg80k       80000   120000    30    0.0    1079270    1400885  1.2980  29.80%  ok

Positive   geomean ratio = 1.1009  (n=2)
Signed     geomean ratio = 1.2597  (n=4)
Overall    geomean ratio = 1.2044  (n=6)   <- your score; lower is better, 1.0000 would be optimal
```

As you improve `solver.cpp` the `ratio` column falls towards 1. In 30 s the
instructor's reference solver reaches 1.0182–1.0667 per graph (see
[The data](#the-data)).

`note=ok` means a valid cut was written in time. Anything else (`TIMEOUT`,
`INVALID`, `CRASH` and the others in
[What the notes mean](#what-the-notes-mean)) means that instance scores
**2.0**, whatever cut the solver had found. One 2.0 among six ratios costs
as much as making the other five cuts about 12% lighter: a solver at the
reference's level (about 1.04) drops to about 1.16 overall. A valid cut first,
then a heavy one. Manage your clock: the limit is `argv[3]`, and the grader
kills you at 31 s whatever you were about to write.

That is the whole workflow. Repeat step 5 as you improve `solver.cpp`, and
upload the two files whenever you want to see where you stand.

### Iterating quickly

The scored run takes three minutes if your solver uses its whole budget. While
you work, use the dev set, which has the same kinds of graph at 3,600–10,000
vertices and gives each 10 s:

```sh
python3 grade.py --solver ./solver --instances instances_dev.txt
```

The dev set is for checking validity and rough quality. Only `instances.txt`
is scored.

## The rules

Your `solver.cpp` must:

1. Start from `maxcut_foundation.cpp`. You may replace any part of it, but
   keep the command line: `./solver <instance.mc> <output.cut> <time_limit_s>`.
2. Write a **valid cut**: exactly `n` values, each `0` or `1`, the side of
   every vertex in order, whitespace-separated (one per line is fine).
3. Finish within the time limit given as `argv[3]` (30 s on the scored set).
   The grader kills the process one second after the limit; a run that is
   killed scores 2.0 even if a good cut was about to be written.
4. Be C++17 using only the standard library.
5. Be a single file: everything you write lives in `solver.cpp`, with no
   headers of your own. `#include` only standard library headers.
6. Be single-threaded.
7. Stay under 4 GB of memory.
8. Read nothing but the instance file. No cached cuts, no precomputed
   answers in the source, no other files.

`grade.py` enforces rules 2, 3, 6 and 7 automatically. Rules 1, 4, 5 and 8
are checked by reading your `solver.cpp`. Randomised solvers are fine: the
grader runs you once, and what that run produces is your score.

## What to submit

Upload two files to the scoreboard:

**https://maxcut.ccu2026algorithm.workers.dev**

- `solver.cpp`, the whole of your work in that one file
- `result.json`, written by step 5

Nothing else. `result.json` already records your cuts, the bounds, the
score, and the SHA-256 of the `solver.cpp` it was measured from — so upload
the two files from the same run.

### How to upload

1. Open the scoreboard and click **Submit** (top right).
2. Enter your **Student ID** exactly as it appears in the course roster. It is
   shown publicly on the board.
3. Choose your `result.json` and your `solver.cpp`.
4. Click **Submit and view ranking**.

The server re-derives your score from the per-instance cuts in `result.json`
and its own copy of the bounds, ranks on that, then redirects you to the board
with your row highlighted. Your `solver.cpp` is stored for the instructor
only; it is never shown to other students.

- **You may submit as many times as you like** before the deadline. Every
  attempt is kept; your **best (lowest)** score is the one that ranks (the
  *Tries* column counts attempts).
- The board closes at the deadline shown in the header (**6 Dec 2026,
  23:59 Taipei**). Late uploads are refused.
- **Overall / Positive / Signed** switch the ranking key; Overall is the
  official one.
- Uploads that were not produced by `grade.py --json`, that come from the dev
  set, that used a different time limit, or that claim a cut heavier than the
  proven bound are rejected with a message telling you what is wrong.

## How the score works

For each instance:

```
ratio = min(upper_bound / cut_weight, 2.0)
```

where `upper_bound` is a **proven** upper bound on the maximum cut of that
graph (see below). Your score is the geometric mean of the six ratios.
**Lower is better**; 1.0000 is the unreachable floor.

- **A failed instance scores 2.0 and still counts.** An invalid cut, a
  timeout, too much memory, or extra threads on one instance drags your mean
  up; they are never dropped. Running the unmodified foundation on an
  instance (1.09–1.30) is always better than failing it.
- **2.0 is also the cap.** On a signed graph a cut can weigh almost nothing
  or even less than nothing (a random cut weighs about 0). A cut of weight
  0 or below scores 2.0, and so does any cut lighter than half the bound, so
  one bad instance never costs more than a failure.
- Two instances are **POSITIVE** (every weight at least 1: a frustrated
  triangular lattice with heavy-tailed weights and an irregular frustrated
  lattice) and four are **SIGNED** (weights of both signs: three triangular
  lattices with different weight distributions and a random 3-regular graph).
  `grade.py` reports the two sub-means, but the overall geometric mean is
  your score.
- **The bound is not the optimum.** It is the value of the semidefinite
  relaxation, which sits about 2% above the best known cut on the positive
  graphs and 3–7% on the signed ones. A ratio of 1.05 means "at most 5% below
  optimal, probably less". Nobody will reach 1.000; the interesting range is
  roughly 1.02–1.30, so the board shows four decimals.
- **Machine speed matters a little.** A faster laptop gets more search done
  in the same 30 s. The effect is small next to the algorithmic differences,
  and the instructor re-runs the top submissions on one machine before final
  grades.

### What the notes mean

| note | meaning | ratio for that instance |
|---|---|---|
| `ok` | valid cut, within limits | `min(upper_bound / cut_weight, 2.0)`; 2.0 if the cut weighs 0 or less |
| `INVALID` | not exactly `n` values, a value other than 0/1, or junk (`invalid_reason` in result.json says which) | 2.0 |
| `TIMEOUT` | still running one second after the limit | 2.0 |
| `MEMORY` | exceeded 4 GB | 2.0 |
| `THREADS` | used more than one thread | 2.0 |
| `CRASH` / `NOOUTPUT` | non-zero exit, or no output file written | 2.0 |

## The data

Six graphs are scored: two lattices with positive weights and four signed
graphs. All of them are frustrated (no cut can satisfy every edge), and the
weights are spread out — heavy-tailed or bell-shaped — so a few edges matter a
lot more than the rest. A trick that helps on one may not help on another:

| instance | vertices | edges | category | what it is | bound | foundation | reference 30 s | reference 5 min |
|---|---:|---:|---|---|---:|---:|---:|---:|
| `tri400tail` | 160,000 | 480,000 | POSITIVE | triangular lattice on a 400 × 400 torus, heavy-tailed weights 1–100 (most edges light, a few heavy); every triangle must leave one edge uncut | 10,819,248 | 1.0942 | 1.0182 | 1.0176 |
| `dlat400` | 160,000 | 480,000 | POSITIVE | irregular frustrated lattice: the 400 × 400 triangular torus with 10% of its edges deleted and extra mid-range and long-range edges added; degrees 0–12, heavy-tailed weights 1–100 | 10,995,384 | 1.1077 | 1.0238 | 1.0226 |
| `tri300s` | 90,000 | 270,000 | SIGNED | triangular lattice on a 300 × 300 torus, heavy-tailed magnitudes 1–100, each edge negative with probability 1/2 | 2,823,442 | 1.2458 | 1.0409 | 1.0394 |
| `tri200s4` | 40,000 | 120,000 | SIGNED | triangular lattice on a 200 × 200 torus, magnitudes 1 + ⌊100u⁴⌋ (even heavier-tailed: most edges very light), random signs | 1,049,436 | 1.2235 | 1.0300 | 1.0287 |
| `tri200g` | 40,000 | 120,000 | SIGNED | triangular lattice on a 200 × 200 torus, bell-shaped weights: the sum of four uniform draws in [−25, 25], never 0 | 1,039,228 | 1.2730 | 1.0667 | 1.0658 |
| `vbg80k` | 80,000 | 120,000 | SIGNED | random 3-regular graph, the same bell-shaped signed weights as `tri200g` | 1,400,885 | 1.2980 | 1.0492 | 1.0476 |

*foundation* is the unmodified starting code; *reference* is the instructor's
own solver given 30 s and five minutes. Beating the five-minute reference is
possible and earns a badge on the board.

Each instance is two files in `instances/`: `<name>.mc` (the graph) and
`<name>.meta.json` (the bound, the reference cuts, the file's SHA-256, and how
the graph was made). Weights are nonzero integers. The six scored `.mc` files
total 25 MB and are fetched by `scripts/download.sh` from the GitHub Release;
the meta files and the `dev_*` graphs are in the repo.

**The final grading uses fresh graphs** generated by the same code with the
same parameters and a different seed, with bounds computed the same way.
Anything that depends on the exact bytes of the shipped files will not carry
over; anything that depends on the structure of the graphs will.

## File formats

```
<instance.mc>                      <output.cut>
n m                                s_0
u v w                              s_1
u v w                              ...
...                                s_{n-1}
(one edge per line, 0 <= u < v < n,   (n values, each 0 or 1: the side of
 integer weight w != 0, may be          vertex i; whitespace-separated)
 negative)
```

The cut's value is the total weight of the edges whose endpoints are on
different sides, negative weights included. `grade.py` computes it exactly in
64-bit integers.

## Where the bound comes from

The LP relaxation of Max-Cut is useless: with every vertex "half on each
side" it declares every edge cut, an integrality gap of 2. The relaxation
that bites is Goemans and Williamson's **semidefinite** one — the same
relax-then-round idea as the lecture, with vectors instead of fractions —
and its dual has a one-line certificate. Write a cut as `x ∈ {−1, +1}^n`.
Then `cut(x) = ¼ xᵀ L x` with `L` the weighted Laplacian, and for any `u`
with `Σ u_i = 0`,

```
cut(x) = ¼ xᵀ (L + diag(u)) x  ≤  (n/4) · λ_max(L + diag(u))
```

because every `x_i² = 1`. `tools/bound/bound.cpp` solves the semidefinite
relaxation approximately (a low-rank "mixing method"), reads off the best
`u` from its optimality conditions, and computes `λ_max` by Lanczos with a
safety margin. The result is a valid bound whatever the optimiser did — the
optimisation only makes it tight. Nothing here needs the weights to be
positive, so the same bound covers the signed graphs (it is also never above
the sum of the positive weights, which no cut can exceed). You do not need it
to compete, but the same vectors, rounded by a random hyperplane, are the
0.878-approximation on the positive graphs (the guarantee needs nonnegative
weights) and a good starting point for a local search on all of them.

## Everything else (optional reading)

**Sanity-check your environment** before you start:

```sh
make foundation && ./foundation samples/tiny.mc /tmp/tiny.cut 5 && cat /tmp/tiny.cut
```

It should print five 0/1 values and `cut 15 of total weight 17` on stderr
(that is the optimum for the sample: sides `1 0 1 1 0`, or the mirror image).

**Practice instances.** `tools/gen/` is the generator that made the shipped
graphs. To make fresh graphs of the same families with your own seed and
bound them:

```sh
make tools                                          # builds tools/gen/gen and tools/bound/bound
python3 -m tools.gen --instance tri300s --seed 12345 --out mydata
tools/bound/bound mydata/tri300s.mc --json          # prints {"upper_bound": ...}
```

Put the `upper_bound` into `mydata/tri300s.meta.json`, list the instance in
your own instances file in the same `<category> <file.mc> <seconds>` format,
and point `--instances` at it.

**`tools/` is instructor tooling.** It is not part of your submission and is
not subject to the rules above.

**Keeping your cuts.** `python3 grade.py ... --keep-cuts cuts/` saves every
cut the grader accepted.

| make target | what it does |
|---|---|
| `make foundation` | build the baseline |
| `make instances` | same as `scripts/download.sh` |
| `make solver` | build your `solver.cpp` |
| `make tools` | build the generator and the upper-bound tool |
| `make test` | run `grade.py`'s own regression tests |
| `make check-data` | verify every instance against its meta file (slow) |
