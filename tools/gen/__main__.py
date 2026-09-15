#!/usr/bin/env python3
"""Generate the competition's graphs from the manifest.

    python3 -m tools.gen                                   # everything in instances/
    python3 -m tools.gen --instance reg800k --seed 12345 --out mydata

Students get the scored graphs from the GitHub Release (scripts/download.sh);
this is the instructor's tool that made them, and the way to make practice
graphs of the same families with your own seed. It calls the C++ generator
tools/gen/gen (integer arithmetic only, so every machine produces the same
bytes). When the manifest's own seed is used, the SHA-256 of each generated
file is checked against checksums/*.sha256.

A <name>.meta.json is written beside each <name>.mc with the parameters and
the digest. The proven upper bound and the reference cut are added by
tools/pipeline.py (instructor) and are already in the committed meta files;
this script keeps those fields when the regenerated digest matches.
"""

import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
GEN = HERE / "gen"


def sha256_file(path, chunk=1 << 20):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(chunk)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def expected_digests():
    out = {}
    for f in (ROOT / "checksums").glob("*.sha256"):
        for line in f.read_text().splitlines():
            parts = line.split()
            if len(parts) == 2:
                out[parts[1]] = parts[0]
    return out


def generate(spec, seed, out_dir, check):
    name = spec["name"]
    out_dir.mkdir(parents=True, exist_ok=True)
    mc = out_dir / f"{name}.mc"
    meta_path = out_dir / f"{name}.meta.json"
    params = [f"{k}={v}" for k, v in spec["params"].items()]
    r = subprocess.run([str(GEN), spec["family"], str(seed), str(mc), *params],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr, file=sys.stderr)
        raise SystemExit(f"generator failed for {name}")
    stats = json.loads(r.stdout.strip().splitlines()[-1])
    digest = sha256_file(mc)

    old = json.loads(meta_path.read_text()) if meta_path.exists() else {}
    meta = {
        "name": name,
        "family": spec["family"],
        "category": spec["category"],
        "time_limit_s": spec["time_limit_s"],
        "seed": seed,
        "params": spec["params"],
        "description": spec.get("description", ""),
        "n": stats["n"], "m": stats["m"],
        "total_weight": stats["total_weight"],
        "weighted": stats["wmax"] != stats["wmin"],
        "deg_min": stats["deg_min"], "deg_max": stats["deg_max"],
        "instance_sha256": digest,
    }
    if old.get("instance_sha256") == digest:
        # keep the bound and reference fields the instructor's pipeline added
        for k, v in old.items():
            meta.setdefault(k, v)
    meta_path.write_text(json.dumps(meta, indent=2) + "\n")

    status = ""
    if check is not None:
        want = check.get(f"{name}.mc")
        if want is None:
            status = "  (no checksum on record)"
        elif want == digest:
            status = "  checksum ok"
        else:
            raise SystemExit(
                f"{mc}: SHA-256 {digest[:16]}... does not match checksums/ ({want[:16]}...).\n"
                "The generator built on this machine does not reproduce the shipped graph. "
                "Do not edit anything; tell the instructor which OS and compiler you use.")
    print(f"wrote {mc}  ({spec['family']}, n={stats['n']:,}, m={stats['m']:,}){status}")
    return mc


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--manifest", type=Path, default=HERE.parent / "manifest" / "public.json")
    ap.add_argument("--out", type=Path, default=ROOT / "instances")
    ap.add_argument("--instance", help="only this instance from the manifest")
    ap.add_argument("--seed", type=int, help="override the manifest seed (then no checksum check)")
    args = ap.parse_args(argv)

    if not GEN.exists():
        raise SystemExit(f"{GEN} is not built; run:  make tools")
    manifest = json.loads(args.manifest.read_text())
    seed = args.seed if args.seed is not None else manifest["seed"]
    check = expected_digests() if args.seed is None else None
    for spec in manifest["instances"]:
        if args.instance and spec["name"] != args.instance:
            continue
        generate(spec, seed, args.out, check)
    return 0


if __name__ == "__main__":
    sys.exit(main())
