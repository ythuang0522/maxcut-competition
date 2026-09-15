#!/usr/bin/env python3
"""
grade.py -- score a submission for the Max-Cut Competition.

Per instance:
  1. Run the solver once, under the instance's wall-clock limit, the 4 GB
     memory cap and the single-thread rule.
  2. Check the output is a valid cut: exactly n values, each 0 or 1, the side
     of every vertex.
  3. Add up the weights of the edges whose endpoints are on different sides
     and divide the instance's PROVEN upper bound by that:

         ratio = upper_bound / cut_weight        (>= 1.0; lower is better)

     A failed instance (timeout, crash, invalid cut, memory, threads)
     scores FAIL_RATIO = 2.0 and still counts towards the mean.

Aggregate: geometric mean of the ratios per category and overall.
**Lower is better.** 1.000 would mean provably optimal on every instance.

Usage:
  python3 grade.py --solver ./solver --instances instances.txt
  python3 grade.py --solver ./solver --instances instances.txt --json result.json

where instances.txt has one line per instance:
   <category> <instance.mc> <time_limit_seconds>

Categories: RANDOM (random regular, Erdos-Renyi, power-law) or STRUCTURED
(triangular lattice, geometric, planted communities). The `<instance>.meta.json`
beside each `.mc` carries the upper bound; without it the instance cannot be
scored.

--- Why an upper bound and not the optimum ------------------------------------

Nobody knows the maximum cut of a graph with two million edges. What we do
have is a PROVEN upper bound on it: the semidefinite-programming relaxation
(Goemans-Williamson), certified through its dual as (n/4) * lambda_max of a
corrected Laplacian (tools/bound). On these graphs it sits a few percent above
the best cut anyone has found, so a ratio of 1.05 means "at most 5% below
optimal, probably less". The bound is the same number for everybody on a given
instance, so it scales every student's ratio identically and cancels out of
the ranking. What does NOT cancel is your machine's speed: a faster laptop
gets more search done in the same 60 seconds. The instructor re-runs the top
submissions on one machine.
"""

import argparse
import hashlib
import json
import math
import os
import resource
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
from array import array
from collections import defaultdict
from pathlib import Path

RANDOM_CATEGORIES = {"RANDOM"}
STRUCTURED_CATEGORIES = {"STRUCTURED"}
ALL_CATEGORIES = RANDOM_CATEGORIES | STRUCTURED_CATEGORIES

FAIL_RATIO = 2.0                      # a failed instance scores this
MEM_CAP_BYTES = 4 << 30               # the 4 GB rule
TIME_GRACE_S = 1.0                    # process start-up slop past the limit
CPU_WALL_RATIO = 1.4                  # single-thread rule: cpu <= 1.4 * wall

SOLVER_SRC = "solver.cpp"


# --------------------------------------------------------------- running ----

class RunResult:
    def __init__(self, wall, cpu, maxrss, status, stderr=""):
        self.wall = wall
        self.cpu = cpu
        self.maxrss = maxrss
        self.status = status        # ok | timeout | crash | memory | threads | nooutput
        self.stderr = stderr

    @property
    def ok(self):
        return self.status == "ok"


_RSS_UNIT = 1 if sys.platform == "darwin" else 1024   # ru_maxrss: bytes vs KiB


class _Footprint:
    """Live memory footprint of a child on macOS, via proc_pid_rusage.

    macOS refuses to lower RLIMIT_AS, and ru_maxrss under-reports once the
    compressor kicks in, so sample phys_footprint while polling and kill a
    child that crosses the cap. Elsewhere RLIMIT_AS does the job.
    """
    _V2, _SIZE, _OFF = 2, 160, 72

    def __init__(self):
        self.lib = None
        if sys.platform == "darwin":
            try:
                import ctypes, ctypes.util
                self.lib = ctypes.CDLL(ctypes.util.find_library("proc") or "libproc.dylib")
                self.buf = ctypes.create_string_buffer(self._SIZE)
            except OSError:
                self.lib = None

    def sample(self, pid):
        if self.lib is None:
            return 0
        if self.lib.proc_pid_rusage(pid, self._V2, self.buf) != 0:
            return 0
        return int.from_bytes(self.buf.raw[self._OFF:self._OFF + 8], "little")


_FOOTPRINT = _Footprint()


def time_run(binary, instance, out, time_limit, enforce_limits=True):
    """Run the solver once: ./solver <instance> <out> <time_limit>."""
    errfd_r, errfd_w = os.pipe()
    t0 = time.perf_counter()
    pid = os.fork()
    if pid == 0:                                   # child
        try:
            os.close(errfd_r)
            devnull = os.open(os.devnull, os.O_WRONLY)
            os.dup2(devnull, 1)
            os.dup2(errfd_w, 2)
            os.close(errfd_w)
            if enforce_limits:
                try:
                    resource.setrlimit(resource.RLIMIT_AS, (MEM_CAP_BYTES, MEM_CAP_BYTES))
                except (OSError, ValueError):
                    pass                            # macOS: footprint sampling instead
            os.execv(str(binary), [str(binary), str(instance), str(out), str(int(time_limit))])
        except BaseException:
            os._exit(127)
    os.close(errfd_w)

    chunks = []

    def drain():
        with os.fdopen(errfd_r, "rb") as f:
            chunks.append(f.read())

    t = threading.Thread(target=drain, daemon=True)
    t.start()

    deadline = t0 + time_limit + TIME_GRACE_S
    status = rusage = None
    killed = over_cap = False
    peak_footprint = 0
    while True:
        done, st, ru = os.wait4(pid, os.WNOHANG)
        if done:
            status, rusage = st, ru
            break
        now = time.perf_counter()
        if enforce_limits:
            peak_footprint = max(peak_footprint, _FOOTPRINT.sample(pid))
        if now > deadline or (enforce_limits and peak_footprint > MEM_CAP_BYTES):
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            _, _, rusage = os.wait4(pid, 0)
            killed = now > deadline
            over_cap = not killed
            break
        time.sleep(0.002 if now - t0 < 2.0 else 0.02)
    t1 = time.perf_counter()
    t.join(timeout=5)
    err = (chunks[0] if chunks else b"").decode(errors="replace")

    wall = t1 - t0
    if killed:
        return RunResult(wall, 0.0, 0, "timeout", err)
    cpu = rusage.ru_utime + rusage.ru_stime
    rss = max(rusage.ru_maxrss * _RSS_UNIT, peak_footprint)
    if over_cap:
        return RunResult(wall, cpu, rss, "memory", err)
    exited_ok = os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0
    if not exited_ok:
        near_cap = enforce_limits and rss >= MEM_CAP_BYTES * 0.9
        return RunResult(wall, cpu, rss, "memory" if near_cap else "crash", err)
    if enforce_limits and rss > MEM_CAP_BYTES:
        return RunResult(wall, cpu, rss, "memory", err)
    if enforce_limits and wall > 1.0 and cpu > CPU_WALL_RATIO * wall:
        return RunResult(wall, cpu, rss, "threads", err)
    if not Path(out).exists():
        return RunResult(wall, cpu, rss, "nooutput", err)
    return RunResult(wall, cpu, rss, "ok", err)


# ------------------------------------------------------------- the cut ----

def sha256_file(path, chunk=1 << 20):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(chunk)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def read_instance(path):
    """(n, m, us, vs, ws): the graph as three parallel arrays of edges."""
    with open(path) as f:
        head = f.readline().split()
        if len(head) != 2:
            raise ValueError(f"{path}: bad header")
        n, m = int(head[0]), int(head[1])
        us, vs, ws = array("i"), array("i"), array("q")
        for line in f:
            toks = line.split()
            if not toks:
                continue
            if len(toks) != 3:
                raise ValueError(f"{path}: malformed edge line {line[:40]!r}")
            u, v, w = int(toks[0]), int(toks[1]), int(toks[2])
            if not (0 <= u < n and 0 <= v < n) or u == v:
                raise ValueError(f"{path}: edge ({u}, {v}) is not a valid edge")
            us.append(u)
            vs.append(v)
            ws.append(w)
    if len(us) != m:
        raise ValueError(f"{path}: header promises {m} edges, file has {len(us)}")
    return n, m, us, vs, ws


def verify_cut(cut_path, n, us, vs, ws):
    """(cut_weight, None) for a valid cut, else (None, reason)."""
    try:
        with open(cut_path, "rb") as f:
            data = f.read()
    except OSError as e:
        return None, f"cannot read output: {e}"
    toks = data.split()
    if not toks:
        return None, "empty output"
    if len(toks) != n:
        return None, f"expected {n} values (one per vertex), got {len(toks)}"
    side = bytearray(n)
    for i, tok in enumerate(toks):
        if tok == b"1":
            side[i] = 1
        elif tok != b"0":
            return None, f"value {i + 1} is not 0 or 1: {tok[:20]!r}"
    total = 0
    for u, v, w in zip(us, vs, ws):
        if side[u] != side[v]:
            total += w
    return total, None


def load_meta(mc_path):
    p = Path(str(mc_path)[:-3] + ".meta.json") if str(mc_path).endswith(".mc") \
        else Path(str(mc_path) + ".meta.json")
    if not p.exists():
        return None, None
    return json.loads(p.read_text()), p


# --------------------------------------------------------------- scoring ----

def geomean(xs):
    xs = [x for x in xs if x > 0]
    if not xs:
        return 0.0
    return math.exp(sum(math.log(x) for x in xs) / len(xs))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--solver", required=True, type=Path)
    ap.add_argument("--instances", required=True, type=Path)
    ap.add_argument("--no-limits", action="store_true",
                    help="skip the memory and thread checks (debugging only)")
    ap.add_argument("--json", type=Path, help="write machine-readable results here")
    ap.add_argument("--keep-cuts", type=Path,
                    help="directory to keep every cut the solver wrote")
    ap.add_argument("--source", type=Path, default=Path(SOLVER_SRC),
                    help="your solver source; its digest goes into --json so "
                         f"the source and the result travel together (default {SOLVER_SRC})")
    args = ap.parse_args()
    args.solver = args.solver.resolve()
    if not args.solver.exists():
        sys.exit(f"solver not found: {args.solver}  (make solver)")

    instances = []
    for line in args.instances.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) != 3 or parts[0] not in ALL_CATEGORIES:
            sys.exit(f"bad instances line (want: <category> <file.mc> <seconds>, "
                     f"category one of {sorted(ALL_CATEGORIES)}): {line}")
        instances.append((parts[0], Path(parts[1]), float(parts[2])))

    missing = [str(p) for _, p, _ in instances if not p.exists()]
    if missing:
        sys.exit("missing instance files (run:  make instances):\n  " + "\n  ".join(missing))

    ratios_by_cat = defaultdict(list)
    rows, records = [], []

    if args.keep_cuts:
        args.keep_cuts.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        for i, (cat, mc, limit) in enumerate(instances):
            meta, meta_path = load_meta(mc)
            name = mc.name[:-3] if mc.name.endswith(".mc") else mc.name
            rec = {"category": cat, "instance": str(mc), "name": name,
                   "time_limit_s": limit}
            if meta is None or "upper_bound" not in meta:
                sys.exit(f"{mc}: no .meta.json with an upper_bound beside it; "
                         "run tools/bound/bound on your own instances first (see README)")
            n, m, us, vs, ws = read_instance(mc)
            ub = int(meta["upper_bound"])
            if meta.get("n") is not None and n != meta["n"]:
                sys.exit(f"{mc}: meta says n={meta['n']} but the file has n={n}")
            if meta.get("m") is not None and m != meta["m"]:
                sys.exit(f"{mc}: meta says m={meta['m']} but the file has {m} edges")
            if meta.get("instance_sha256") and meta["instance_sha256"] != sha256_file(mc):
                sys.exit(f"{mc}: the file's digest does not match its meta.json -- "
                         "regenerate it (make instances)")
            rec.update(n=n, m=m, upper_bound=ub,
                       ref_cut=meta.get("ref_cut"),
                       meta_sha256=sha256_file(meta_path))

            out = td / f"{i}.cut"
            r = time_run(args.solver, mc, out, limit,
                         enforce_limits=not args.no_limits)
            if r.stderr.strip():
                rec["stderr"] = r.stderr.strip()[-2000:]

            note, ratio, cut = None, FAIL_RATIO, None
            if not r.ok:
                note = r.status.upper()
            else:
                cut, why = verify_cut(out, n, us, vs, ws)
                if cut is None:
                    note = "INVALID"
                    rec["invalid_reason"] = why
                else:
                    ratio = ub / cut if cut > 0 else FAIL_RATIO
                    note = "ok"
                    rec["cut_sha256"] = sha256_file(out)
                    if args.keep_cuts:
                        shutil.copy(out, args.keep_cuts / f"{name}.cut")

            ratios_by_cat[cat].append(ratio)
            rows.append((cat, name, n, m, limit, r.wall, cut, ub, ratio, note))
            rec.update(cut=cut, ratio=ratio, note=note,
                       wall_s=r.wall, cpu_s=r.cpu, max_rss_bytes=r.maxrss)
            records.append(rec)

    print(f"{'category':<11}{'instance':<10}{'n':>8}{'m':>9}{'limit':>6}{'wall':>7}"
          f"{'cut':>11}{'bound':>11}{'ratio':>8}{'gap':>8}  note")
    print("-" * 106)
    for cat, name, n, m, limit, wall, cut, ub, ratio, note in rows:
        cs = f"{cut:11d}" if cut is not None else f"{'-':>11}"
        gap = f"{(ratio - 1) * 100:7.2f}%" if note == "ok" else f"{'-':>8}"
        print(f"{cat:<11}{name:<10}{n:>8}{m:>9}{limit:6.0f}{wall:7.1f}"
              f"{cs}{ub:11d}{ratio:8.4f}{gap}  {note}")

    rnd = sum((ratios_by_cat[c] for c in RANDOM_CATEGORIES), [])
    stc = sum((ratios_by_cat[c] for c in STRUCTURED_CATEGORIES), [])
    overall = rnd + stc
    print()
    print(f"Random     geomean ratio = {geomean(rnd):.4f}  (n={len(rnd)})")
    print(f"Structured geomean ratio = {geomean(stc):.4f}  (n={len(stc)})")
    print(f"Overall    geomean ratio = {geomean(overall):.4f}  (n={len(overall)})   "
          f"<- your score; lower is better, 1.0000 would be optimal")

    if args.json:
        payload = {
            "competition": "max-cut",
            "solver": str(args.solver),
            "solver_sha256": sha256_file(args.solver),
            "solver_source": str(args.source),
            "solver_source_sha256": (sha256_file(args.source)
                                     if args.source.exists() else None),
            "limits_enforced": not args.no_limits,
            "mem_cap_bytes": MEM_CAP_BYTES,
            "time_grace_s": TIME_GRACE_S,
            "cpu_wall_ratio": CPU_WALL_RATIO,
            "fail_ratio": FAIL_RATIO,
            "instances": records,
            "random_geomean": geomean(rnd),
            "structured_geomean": geomean(stc),
            "overall_geomean": geomean(overall),
        }
        args.json.write_text(json.dumps(payload, indent=2) + "\n")
        print(f"\nwrote {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
