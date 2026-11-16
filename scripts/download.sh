#!/usr/bin/env bash
# Fetch the six scored graphs from the GitHub Release.
#
# Usage:
#   scripts/download.sh
#   RELEASE_TAG=v1.1 scripts/download.sh     # another release (also RELEASE_REPO)
#
# The .mc files arrive gzipped and are decompressed into instances/, so every
# path in the README works unchanged. Each file's SHA-256 is checked against
# checksums/scored.sha256 (the same digest is in its .meta.json, and grade.py
# checks it again before every run). A file already in instances/ is kept only
# if its digest matches; a missing, corrupt or outdated file is fetched again.
# A download that does not match aborts the script and leaves nothing behind,
# rather than leaving you to debug a truncated file as an algorithm bug.

set -euo pipefail

REPO="${RELEASE_REPO:-ythuang0522/maxcut-competition}"
TAG="${RELEASE_TAG:-v1.2}"

cd "$(dirname "$0")/.."
mkdir -p instances

names=(tri400tail dlat400 tri300s tri200s4 tri200g vbg80k)
sums="checksums/scored.sha256"

sha256_of() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
  else shasum -a 256 "$1" | cut -d' ' -f1
  fi
}

want_of() { awk -v f="$1.mc" '$2 == f {print $1}' "$sums"; }

for name in "${names[@]}"; do
  if [[ -z "$(want_of "$name")" ]]; then
    echo "  ${sums} has no digest for ${name}.mc; run  git pull  first" >&2
    exit 1
  fi
done

# Downloads land in a scratch directory and only a verified file is moved
# into place, so an interrupted or bad download never leaves a partial file.
tmp="$(mktemp -d instances/.download.XXXXXX)"
trap 'rm -rf "$tmp"' EXIT

echo "Downloading the scored graphs from ${REPO} @ ${TAG} (about 9 MB compressed, 25 MB on disk) ..."
for name in "${names[@]}"; do
  dest="instances/${name}.mc"
  want="$(want_of "$name")"
  if [[ -f "$dest" ]]; then
    if [[ "$(sha256_of "$dest")" == "$want" ]]; then
      echo "  [ok  ] ${dest} already present and verified"
      continue
    fi
    echo "  [old ] ${dest} does not match checksums/ (outdated or corrupt); fetching it again"
    rm -f "$dest"
  fi
  gz="${tmp}/${name}.mc.gz"
  url="https://github.com/${REPO}/releases/download/${TAG}/${name}.mc.gz"
  echo "  [get ] ${name}.mc"
  if ! curl -fsSL --retry 3 -o "$gz" "$url"; then
    # A private repository (before release day) needs an authenticated download.
    rm -f "$gz"
    if command -v gh >/dev/null 2>&1; then
      gh release download "$TAG" -R "$REPO" -p "${name}.mc.gz" -D "$tmp" --clobber
    else
      echo "  download failed (is the repository public yet? otherwise install gh and run gh auth login)" >&2
      exit 1
    fi
  fi
  if ! gunzip -c "$gz" > "${tmp}/${name}.mc"; then
    echo "  ${name}.mc.gz from ${TAG} is not a valid gzip file; nothing was installed" >&2
    exit 1
  fi
  got="$(sha256_of "${tmp}/${name}.mc")"
  if [[ "$got" != "$want" ]]; then
    echo "  CHECKSUM MISMATCH for ${name}.mc from ${TAG}: got ${got}, want ${want}; nothing was installed" >&2
    exit 1
  fi
  mv -f "${tmp}/${name}.mc" "$dest"
done
echo "Done. instances/ now holds the scored set; run:"
echo "  python3 grade.py --solver ./solver --instances instances.txt --json result.json"
