#!/usr/bin/env bash
# Fetch the six scored graphs from the GitHub Release.
#
# Usage:
#   scripts/download.sh
#
# The .mc files arrive gzipped and are decompressed into instances/, so every
# path in the README works unchanged. Each file's SHA-256 is checked against
# checksums/scored.sha256 (the same digest is in its .meta.json, and grade.py
# checks it again before every run); a mismatch aborts rather than leaving you
# to debug a truncated download as an algorithm bug. Files already present
# are skipped.

set -euo pipefail

REPO="${RELEASE_REPO:-ythuang0522/maxcut-competition}"
TAG="${RELEASE_TAG:-v1.0}"

cd "$(dirname "$0")/.."
mkdir -p instances

names=(reg800k gnm600k pow600k tri900 geo600k comm600k)
sums="checksums/scored.sha256"

sha256_of() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
  else shasum -a 256 "$1" | cut -d' ' -f1
  fi
}

echo "Downloading the scored graphs from ${REPO} @ ${TAG} (about 130 MB compressed, 370 MB on disk) ..."
for name in "${names[@]}"; do
  dest="instances/${name}.mc"
  if [[ -f "$dest" ]]; then
    echo "  [skip] ${dest} already present"
    continue
  fi
  url="https://github.com/${REPO}/releases/download/${TAG}/${name}.mc.gz"
  echo "  [get ] ${name}.mc"
  if ! curl -fsSL --retry 3 -o "${dest}.gz" "$url"; then
    # A private repository (before release day) needs an authenticated download.
    if command -v gh >/dev/null 2>&1; then
      rm -f "${dest}.gz"
      gh release download "$TAG" -R "$REPO" -p "${name}.mc.gz" -D instances --clobber
    else
      echo "  download failed (is the repository public yet? otherwise install gh and run gh auth login)" >&2
      exit 1
    fi
  fi
  gunzip -f "${dest}.gz"
  want=$(awk -v f="${name}.mc" '$2 == f {print $1}' "$sums")
  got=$(sha256_of "$dest")
  if [[ "$got" != "$want" ]]; then
    echo "  CHECKSUM MISMATCH for ${dest}: got ${got}, want ${want}" >&2
    rm -f "$dest"
    exit 1
  fi
done
echo "Done. instances/ now holds the scored set; run:"
echo "  python3 grade.py --solver ./solver --instances instances.txt --json result.json"
