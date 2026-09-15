#!/usr/bin/env python3
"""Instructor pipeline: build instances with proven bounds and reference cuts.

    python3 tools/pipeline.py build --manifest tools/manifest/public.json \
            --out instances --ref-seconds 300 [--only reg400k] [--jobs 3]
    python3 tools/pipeline.py check [--dir instances] [--fast]

`build`, per instance:
  1. tools.gen                    -> <name>.mc, <name>.meta.json (with the
                                     file's SHA-256)
  2. ./foundation                 -> foundation_cut, the baseline row on the
                                     scoreboard
  3. tools/ref/solvers/refsolve   -> <name>.cut (kept in tools/ref/cuts/,
                                     gitignored) and ref_cut; also a run on
                                     the students' budget -> ref60_cut
  4. tools/bound/bound            -> SDP upper bound (n/4 lambda_max), floored
  5. sanity: foundation_cut <= ref_cut <= upper_bound, every cut re-verifies
     with grade.py's own checker, upper_bound/foundation < FAIL_RATIO (so
     failing an instance is always worse than running the foundation). Any
     violation FAILS THE BUILD.
  6. merge into <name>.meta.json; write checksums/scored.sha256, dev.sha256

`check` re-verifies every instance: digest matches the meta, the reference
cut still has the stored value, the stored bound is not below it, and (unless
--fast) the bound tool reproduces the stored bound to within 0.05%.
"""

import argparse
import hashlib
import json
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(ROOT))

from grade import read_instance, verify_cut, sha256_file, FAIL_RATIO  # noqa: E402

BOUND = HERE / "bound" / "bound"
REFSOLVE = HERE / "ref" / "solvers" / "refsolve"
FOUNDATION = ROOT / "foundation"
CUTS = HERE / "ref" / "cuts"
CHECKSUMS = ROOT / "checksums"


def run(cmd, **kw):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, **kw)
    if r.returncode != 0:
        print(r.stdout)
        print(r.stderr)
        raise SystemExit(f"command failed (exit {r.returncode}): {' '.join(str(c) for c in cmd)}")
    return r


def build_one(spec, manifest, out_dir, ref_seconds, reuse_ref=False):
    name = spec["name"]
    mc = out_dir / f"{name}.mc"
    meta_path = out_dir / f"{name}.meta.json"
    log = lambda *a: print(f"[{name}]", *a, flush=True)

    # 1. generate (tools.gen keeps the old bound fields when the digest is unchanged)
    old_meta = json.loads(meta_path.read_text()) if meta_path.exists() else {}
    seed = json.loads(Path(manifest).read_text())["seed"]
    run([sys.executable, "-m", "tools.gen", "--manifest", manifest,
         "--instance", name, "--out", out_dir, "--seed", seed], cwd=ROOT)
    meta = json.loads(meta_path.read_text())
    reusable = reuse_ref and old_meta.get("instance_sha256") == meta.get("instance_sha256")
    if reusable:
        meta.update({k: v for k, v in old_meta.items() if k not in meta})
    n, m, us, vs, ws = read_instance(mc)
    CUTS.mkdir(parents=True, exist_ok=True)

    # 2. foundation
    f_cut_path = CUTS / f"{name}.foundation.cut"
    run([FOUNDATION, mc, f_cut_path, spec["time_limit_s"]])
    f_cut, why = verify_cut(f_cut_path, n, us, vs, ws)
    if f_cut is None:
        raise SystemExit(f"{name}: foundation cut invalid: {why}")
    log(f"foundation {f_cut}")

    # 3. reference (long run) and the same solver on the students' budget
    ref_path = CUTS / f"{name}.cut"
    secs = ref_seconds if not name.startswith("dev_") else max(10, ref_seconds // 10)
    extra = {}
    if reusable and ref_path.exists() and "ref_cut" in meta:
        log("reusing existing reference cut")
        ref_cut, why = verify_cut(ref_path, n, us, vs, ws)
        extra = {k: meta[k] for k in ("ref_seconds",) if k in meta}
    else:
        log(f"refsolve {secs}s ...")
        r = run([REFSOLVE, mc, ref_path, secs, 1])
        rep = json.loads(r.stdout.strip().splitlines()[-1])
        ref_cut, why = verify_cut(ref_path, n, us, vs, ws)
        if ref_cut != rep["cut"]:
            raise SystemExit(f"{name}: refsolve says {rep['cut']}, checker says {ref_cut} ({why})")
        extra = {"ref_seconds": secs}
    if ref_cut is None:
        raise SystemExit(f"{name}: reference cut invalid: {why}")
    log(f"ref {ref_cut}")

    ref60_path = CUTS / f"{name}.ref60.cut"
    if reusable and ref60_path.exists() and "ref60_cut" in meta:
        ref60_cut, why = verify_cut(ref60_path, n, us, vs, ws)
    else:
        log(f"refsolve {spec['time_limit_s']}s (student budget) ...")
        run([REFSOLVE, mc, ref60_path, spec["time_limit_s"], 1])
        ref60_cut, why = verify_cut(ref60_path, n, us, vs, ws)
    if ref60_cut is None:
        raise SystemExit(f"{name}: 60 s reference cut invalid: {why}")
    extra["ref60_cut"] = ref60_cut
    log(f"ref@{spec['time_limit_s']}s {ref60_cut}")

    # 4. upper bound
    if reusable and "upper_bound" in meta and "lambda_max" in meta:
        ub, lam, sdp = meta["upper_bound"], meta["lambda_max"], meta.get("sdp_primal")
        log(f"reusing upper_bound {ub}")
    else:
        log("bound ...")
        r = run([BOUND, mc, "--json"])
        b = json.loads(r.stdout.strip().splitlines()[-1])
        ub, lam, sdp = int(b["upper_bound"]), b["lambda_max"], b["sdp_primal"]
        extra["bound_rank"] = b["rank"]
        extra["bound_sweeps"] = b["sweeps"]
    log(f"upper_bound {ub}  (lambda {lam:.6f}; UB/ref {ub / ref_cut:.4f}, UB/foundation {ub / f_cut:.4f})")

    # 5. sanity
    if not (f_cut <= ref_cut <= ub):
        raise SystemExit(f"{name}: foundation/ref/bound not ordered: {f_cut} {ref_cut} {ub}")
    if ub / f_cut >= FAIL_RATIO:
        raise SystemExit(f"{name}: bound is {ub / f_cut:.3f}x the foundation, not below FAIL_RATIO {FAIL_RATIO} -- "
                         "failing the instance would beat running the foundation; change the instance")

    # 6. merge
    meta.update(
        upper_bound=ub,
        lambda_max=round(lam, 9),
        sdp_primal=round(sdp, 3) if sdp is not None else None,
        foundation_cut=f_cut,
        foundation_ratio=round(ub / f_cut, 5),
        ref_cut=ref_cut,
        ref_ratio=round(ub / ref_cut, 5),
        ref60_ratio=round(ub / extra["ref60_cut"], 5),
        **extra,
    )
    meta_path.write_text(json.dumps(meta, indent=2) + "\n")
    log("meta written")
    return name, meta


def write_checksums(out_dir):
    CHECKSUMS.mkdir(exist_ok=True)
    scored, dev = [], []
    for meta_path in sorted(out_dir.glob("*.meta.json")):
        meta = json.loads(meta_path.read_text())
        line = f"{meta['instance_sha256']}  {meta['name']}.mc\n"
        (dev if meta["name"].startswith("dev_") else scored).append(line)
    (CHECKSUMS / "scored.sha256").write_text("".join(scored))
    (CHECKSUMS / "dev.sha256").write_text("".join(dev))


def cmd_build(args):
    manifest = json.loads(args.manifest.read_text())
    specs = [s for s in manifest["instances"] if not args.only or s["name"] in args.only]
    for exe in (BOUND, REFSOLVE, FOUNDATION, HERE / "gen" / "gen"):
        if not exe.exists():
            raise SystemExit(f"missing {exe}: run  make foundation tools tools/ref/solvers/refsolve")
    with ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = [ex.submit(build_one, s, args.manifest, args.out, args.ref_seconds, args.reuse_ref)
                for s in specs]
        results = [f.result() for f in futs]
    write_checksums(args.out)
    print()
    print(f"{'instance':<12}{'n':>8}{'m':>9}{'bound':>11}{'foundation':>11}{'UB/fnd':>8}"
          f"{'ref60':>11}{'UB/r60':>8}{'ref300':>11}{'UB/ref':>8}")
    for name, mt in results:
        print(f"{name:<12}{mt['n']:>8}{mt['m']:>9}{mt['upper_bound']:>11}{mt['foundation_cut']:>11}{mt['foundation_ratio']:>8.4f}"
              f"{mt['ref60_cut']:>11}{mt['ref60_ratio']:>8.4f}{mt['ref_cut']:>11}{mt['ref_ratio']:>8.4f}")


def cmd_check(args):
    bad = 0
    for meta_path in sorted(args.dir.glob("*.meta.json")):
        meta = json.loads(meta_path.read_text())
        name = meta["name"]
        mc = args.dir / f"{name}.mc"
        ok = True
        if not mc.exists():
            print(f"{name}: instance file missing (make instances)"); print(f"SKIP {name}"); continue
        if sha256_file(mc) != meta["instance_sha256"]:
            print(f"{name}: digest mismatch"); ok = False
        n, m, us, vs, ws = read_instance(mc)
        if n != meta["n"] or m != meta["m"]:
            print(f"{name}: n/m mismatch"); ok = False
        ref_path = CUTS / f"{name}.cut"
        if ref_path.exists() and "ref_cut" in meta:
            c, why = verify_cut(ref_path, n, us, vs, ws)
            if c != meta["ref_cut"]:
                print(f"{name}: reference cut {c} != stored {meta['ref_cut']} ({why})"); ok = False
        if "upper_bound" in meta and meta["upper_bound"] < meta.get("ref_cut", 0):
            print(f"{name}: upper_bound below ref_cut"); ok = False
        if not args.fast and "lambda_max" in meta:
            r = run([BOUND, mc, "--json"])
            b = json.loads(r.stdout.strip().splitlines()[-1])
            rel = abs(b["lambda_max"] - meta["lambda_max"]) / max(1e-9, abs(meta["lambda_max"]))
            if rel > 5e-4:
                print(f"{name}: recomputed lambda {b['lambda_max']:.6f} vs stored {meta['lambda_max']:.6f}"); ok = False
        print(f"{'ok   ' if ok else 'BAD  '}{name}")
        bad += not ok
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build")
    b.add_argument("--manifest", type=Path, default=HERE / "manifest" / "public.json")
    b.add_argument("--out", type=Path, default=ROOT / "instances")
    b.add_argument("--only", nargs="*")
    b.add_argument("--ref-seconds", type=int, default=300)
    b.add_argument("--reuse-ref", action="store_true",
                   help="keep existing reference cuts and bounds when the instance is unchanged")
    b.add_argument("--jobs", type=int, default=3)
    c = sub.add_parser("check")
    c.add_argument("--dir", type=Path, default=ROOT / "instances")
    c.add_argument("--fast", action="store_true", help="skip recomputing the bounds")
    args = ap.parse_args()
    return cmd_build(args) if args.cmd == "build" else cmd_check(args)


if __name__ == "__main__":
    sys.exit(main() or 0)
