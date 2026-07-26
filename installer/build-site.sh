#!/usr/bin/env bash
#
# Assemble the web installer site: the page, one manifest and firmware binary
# per offered release, and a versions.json index the page reads to build its
# version picker.
#
#   ./installer/build-site.sh --out site                 # latest 5 releases
#   ./installer/build-site.sh --out site --limit 2
#   ./installer/build-site.sh --out site --bin build/x.bin --version dev
#
# Used by both .github/workflows/pages.yml and installer/preview.sh, so what
# you preview locally is exactly what gets deployed.
set -euo pipefail

REPO="${REPO:-marklynch/pool-controller-code}"
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LIMIT=5
OUT=""
LOCAL_BIN=""
LOCAL_VERSION=""

while [ $# -gt 0 ]; do
  case "$1" in
    --out) OUT="$2"; shift 2 ;;
    --limit) LIMIT="$2"; shift 2 ;;
    --bin) LOCAL_BIN="$2"; shift 2 ;;
    --version) LOCAL_VERSION="$2"; shift 2 ;;
    -h|--help) sed -n '2,12p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 1 ;;
  esac
done

[ -n "$OUT" ] || { echo "--out is required" >&2; exit 1; }

rm -rf "$OUT"
mkdir -p "$OUT"

# Collected as "tag<TAB>published" lines, newest first.
SELECTED=""

if [ -n "$LOCAL_BIN" ]; then
  [ -f "$LOCAL_BIN" ] || { echo "no such file: $LOCAL_BIN" >&2; exit 1; }
  VERSION="${LOCAL_VERSION:-local}"
  cp "$LOCAL_BIN" "$OUT/pool-controller-full-${VERSION}.bin"
  SELECTED="${VERSION}	"
else
  command -v gh >/dev/null || {
    echo "gh CLI not found — install it, or pass --bin <path> for a local build." >&2
    exit 1
  }

  # Over-fetch: drafts, prereleases and any release predating the merged
  # full-flash asset are skipped, and we still want LIMIT usable ones.
  CANDIDATES=$(gh release list --repo "$REPO" --limit $((LIMIT * 3)) \
    --json tagName,publishedAt,isDraft,isPrerelease \
    -q '.[] | select(.isDraft == false and .isPrerelease == false)
        | "\(.tagName)\t\(.publishedAt)"')

  count=0
  while IFS=$'\t' read -r tag published; do
    [ -n "$tag" ] || continue
    [ "$count" -lt "$LIMIT" ] || break
    binary="pool-controller-full-${tag}.bin"
    if gh release download "$tag" --repo "$REPO" --pattern "$binary" --dir "$OUT" 2>/dev/null; then
      SELECTED="${SELECTED}${tag}	${published}"$'\n'
      count=$((count + 1))
      echo "  + ${tag} ($(du -h "$OUT/$binary" | cut -f1))"
    else
      echo "  - ${tag} skipped (no ${binary})" >&2
    fi
  done <<< "$CANDIDATES"

  [ "$count" -gt 0 ] || { echo "no releases with a full-flash binary found" >&2; exit 1; }
fi

# One manifest per version. ESP Web Tools reads the manifest at click time, so
# the page just swaps the button's manifest attribute when the picker changes.
while IFS=$'\t' read -r tag published; do
  [ -n "$tag" ] || continue
  sed -e "s|__VERSION__|${tag}|g" \
      -e "s|__BINARY__|pool-controller-full-${tag}.bin|g" \
      "$SRC/manifest.template.json" > "$OUT/manifest-${tag}.json"
  if grep -q '__VERSION__\|__BINARY__' "$OUT/manifest-${tag}.json"; then
    echo "manifest-${tag}.json still contains unsubstituted placeholders" >&2
    exit 1
  fi
  test -s "$OUT/pool-controller-full-${tag}.bin"
done <<< "$SELECTED"

LATEST=$(printf '%s' "$SELECTED" | head -n1 | cut -f1)

REPO="$REPO" LATEST="$LATEST" SELECTED="$SELECTED" OUT="$OUT" python3 - <<'PY'
import json, os

repo = os.environ["REPO"]
out = os.environ["OUT"]
versions = []
for line in os.environ["SELECTED"].splitlines():
    if not line.strip():
        continue
    tag, _, published = line.partition("\t")
    versions.append({
        "version": tag,
        "manifest": f"manifest-{tag}.json",
        "published": published[:10],
        "notes": f"https://github.com/{repo}/releases/tag/{tag}",
    })

with open(os.path.join(out, "versions.json"), "w") as f:
    json.dump({"latest": os.environ["LATEST"], "versions": versions}, f, indent=2)
    f.write("\n")
PY

cp "$SRC/index.html" "$OUT/index.html"
# Kept so a direct link to manifest.json still resolves to the current release.
cp "$OUT/manifest-${LATEST}.json" "$OUT/manifest.json"

echo "Built $OUT — latest ${LATEST}, $(grep -c '"version"' "$OUT/versions.json") version(s)."
