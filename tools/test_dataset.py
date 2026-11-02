#!/usr/bin/env python3
"""Three checks of the shipped dataset (v1.1).

    make dataset-check            # or: make tools && python3 tools/test_dataset.py

  1. the generator rebuilds the two graphs adopted in v1.1, tri400tail and
     dlat400, byte for byte from tools/manifest/public.json
  2. scripts/download.sh keeps verified files, fetches a corrupt and a missing
     one again, and aborts on a bad download without leaving a file behind
     (offline: curl is replaced by a stub)
  3. instances.txt, the manifest, the meta files, checksums/ and
     scripts/download.sh name the same six scored graphs, and every stored
     bound sits above the reference and foundation cuts
"""

import gzip
import hashlib
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
from grade import FAIL_RATIO  # noqa: E402

GEN = ROOT / "tools" / "gen" / "gen"
SCORED = ["reg200k", "regw200k", "gnm200k", "tri400tail", "dlat400", "geo20k"]
RETIRED = ["tri400", "tri900"]
# The two graphs adopted in v1.1, as measured when they were selected.
ADOPTED_SHA256 = {
    "tri400tail": "3e8825087117caea09ad4a29bd4d90fd288e33ac62cdd8e0174013a722a349e4",
    "dlat400": "2caa57c71a2983afeb76a8f82bbc6485824fa9ddc88432c661136326a944082d",
}


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def read_sums(path):
    return {p[1]: p[0] for p in (l.split() for l in Path(path).read_text().splitlines()) if len(p) == 2}


def test_adopted_pair_regenerates():
    """1. tri400tail and dlat400 regenerate byte for byte."""
    if not GEN.exists():
        raise SystemExit(f"{GEN} is not built; run:  make tools")
    scored = read_sums(ROOT / "checksums" / "scored.sha256")
    bad = []
    with tempfile.TemporaryDirectory() as tmp:
        for name, want in ADOPTED_SHA256.items():
            r = subprocess.run([sys.executable, "-m", "tools.gen", "--instance", name, "--out", tmp],
                               cwd=ROOT, capture_output=True, text=True)
            mc = Path(tmp) / f"{name}.mc"
            got = sha256(mc) if r.returncode == 0 and mc.exists() else (r.stderr or r.stdout).strip()[-200:]
            if not (got == want == scored.get(f"{name}.mc")):
                bad.append(f"{name}: {got[:16]}")
    return bad


FAKE_CURL = r"""#!/bin/bash
# curl stub: -o <dest> <url>; serves $FAKE_ASSETS/<basename of url>, logs the url
dest=""; url=""
while [[ $# -gt 0 ]]; do
  case "$1" in -o) dest="$2"; shift 2;; -*) shift;; *) url="$1"; shift;; esac
done
echo "$url" >> "$FAKE_LOG"
src="$FAKE_ASSETS/$(basename "$url")"
[[ -f "$src" ]] || exit 22
cp "$src" "$dest"
"""


def test_download_recovers_and_aborts():
    """2. download.sh: reuse verified files, re-fetch bad ones, abort on a bad asset."""
    bad = []
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        repo, assets, fbin, inst = tmp / "repo", tmp / "assets", tmp / "bin", tmp / "repo" / "instances"
        for d in (repo / "scripts", repo / "checksums", inst, assets, fbin):
            d.mkdir(parents=True)
        shutil.copy(ROOT / "scripts" / "download.sh", repo / "scripts" / "download.sh")
        bodies = {name: f"2 1\n0 1 {i + 1}\n".encode() for i, name in enumerate(SCORED)}
        for name, body in bodies.items():
            (assets / f"{name}.mc.gz").write_bytes(gzip.compress(body))
            (inst / f"{name}.mc").write_bytes(body)                      # a verified cache
        (repo / "checksums" / "scored.sha256").write_text(
            "".join(f"{hashlib.sha256(b).hexdigest()}  {n}.mc\n" for n, b in bodies.items()))
        (fbin / "curl").write_text(FAKE_CURL)
        (fbin / "curl").chmod(0o755)
        log = tmp / "urls.log"
        env = {"PATH": f"{fbin}:/usr/bin:/bin", "FAKE_ASSETS": str(assets), "FAKE_LOG": str(log),
               "HOME": str(tmp)}

        def run():
            log.write_text("")
            r = subprocess.run(["/bin/bash", str(repo / "scripts" / "download.sh")],
                               env=env, capture_output=True, text=True)
            return r, log.read_text().split()

        # an outdated file and a missing file: only these two are fetched
        (inst / "dlat400.mc").write_text("2 1\n0 1 999\n")
        (inst / "geo20k.mc").unlink()
        r, urls = run()
        if r.returncode != 0:
            bad.append(f"recovery exit {r.returncode}: {r.stderr.strip()[-200:]}")
        if sorted(Path(u).name for u in urls) != ["dlat400.mc.gz", "geo20k.mc.gz"]:
            bad.append(f"recovery fetched {urls}")
        if not all("/releases/download/v1.1/" in u for u in urls):
            bad.append("recovery did not use release v1.1")
        if any(sha256(inst / f"{n}.mc") != hashlib.sha256(b).hexdigest() for n, b in bodies.items()):
            bad.append("installed files do not match checksums")

        # a release asset with the wrong content: abort, install nothing
        (assets / "tri400tail.mc.gz").write_bytes(gzip.compress(b"2 1\n0 1 7\n"))
        (inst / "tri400tail.mc").unlink()
        r, _ = run()
        if r.returncode == 0 or "CHECKSUM MISMATCH" not in r.stderr:
            bad.append(f"bad asset not refused (exit {r.returncode})")
        if list(inst.glob("tri400tail.mc*")):
            bad.append("bad asset left a file behind")
    return bad


def test_scored_set_is_coherent():
    """3. the same six scored graphs everywhere; bounds above the cuts."""
    bad = []
    manifest = [s for s in json.loads((ROOT / "tools" / "manifest" / "public.json").read_text())["instances"]
                if not s["name"].startswith("dev_")]
    rows = [l.split() for l in (ROOT / "instances.txt").read_text().splitlines()
            if l.strip() and not l.startswith("#")]
    scored = read_sums(ROOT / "checksums" / "scored.sha256")
    dl = (ROOT / "scripts" / "download.sh").read_text()
    m = re.search(r"^names=\(([^)]*)\)", dl, re.M)
    metas = sorted(p.name[:-len(".meta.json")] for p in (ROOT / "instances").glob("*.meta.json")
                   if not p.name.startswith("dev_"))
    views = {
        "instances.txt": [Path(r[1]).stem for r in rows],
        "manifest": [s["name"] for s in manifest],
        "checksums/scored.sha256": [Path(n).stem for n in scored],
        "download.sh": m.group(1).split() if m else [],
        "meta files": metas,
    }
    for where, names in views.items():
        if sorted(names) != sorted(SCORED):
            bad.append(f"{where}: {names}")
    if [r[0] for r in rows] != ["RANDOM"] * 3 + ["STRUCTURED"] * 3 or {r[2] for r in rows} != {"60"}:
        bad.append("instances.txt: not 3 RANDOM + 3 STRUCTURED at 60 s")
    if "RELEASE_TAG:-v1.1}" not in dl:
        bad.append("download.sh does not default to v1.1")
    for name in SCORED:
        p = ROOT / "instances" / f"{name}.meta.json"
        if not p.exists():
            continue
        mt = json.loads(p.read_text())
        f, r60, ref, ub = mt["foundation_cut"], mt["ref60_cut"], mt["ref_cut"], mt["upper_bound"]
        if mt["instance_sha256"] != scored.get(f"{name}.mc"):
            bad.append(f"{name}: meta digest differs from checksums")
        if not (f <= r60 <= ub and f <= ref <= ub and ub / f < FAIL_RATIO):
            bad.append(f"{name}: cuts/bound out of order ({f} {r60} {ref} {ub})")
    for f in ("instances.txt", "scripts/download.sh", "checksums/scored.sha256"):
        for w in RETIRED:
            if re.search(rf"\b{w}\b", (ROOT / f).read_text()):
                bad.append(f"{f} still names {w}")
    return bad


def main():
    tests = (test_adopted_pair_regenerates, test_download_recovers_and_aborts, test_scored_set_is_coherent)
    failed = 0
    for t in tests:
        bad = t()
        failed += bool(bad)
        print(f"{'PASS' if not bad else 'FAIL'}  {t.__doc__}" + "".join(f"\n        {b}" for b in bad))
    print(f"\n{len(tests) - failed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
