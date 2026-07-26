#!/usr/bin/env bash
#
# Preview the web installer locally, exactly as GitHub Pages will serve it.
#
#   ./installer/preview.sh              # latest release
#   ./installer/preview.sh v1.11.0      # a specific release
#   ./installer/preview.sh --bin build/pool-controller-full.bin
#
# Assembles the same site/ layout the deploy workflow builds — page, stamped
# manifest and firmware binary together — and serves it over http://localhost,
# which counts as a secure context, so the flasher genuinely works here.
#
# Opening installer/index.html straight from disk does NOT work: there is no
# manifest.json in the repo (it is generated), and file:// can't fetch it.
set -euo pipefail

REPO="marklynch/pool-controller-code"
PORT="${PORT:-8000}"
OUT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/.installer-preview"
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

LOCAL_BIN=""
VERSION=""
while [ $# -gt 0 ]; do
  case "$1" in
    --bin) LOCAL_BIN="$2"; shift 2 ;;
    -h|--help) sed -n '2,14p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) VERSION="$1"; shift ;;
  esac
done

rm -rf "$OUT"
mkdir -p "$OUT"

if [ -n "$LOCAL_BIN" ]; then
  [ -f "$LOCAL_BIN" ] || { echo "no such file: $LOCAL_BIN" >&2; exit 1; }
  VERSION="${VERSION:-local}"
  BINARY="pool-controller-full-${VERSION}.bin"
  cp "$LOCAL_BIN" "$OUT/$BINARY"
else
  command -v gh >/dev/null || {
    echo "gh CLI not found — install it, or pass --bin <path> to use a local build." >&2
    exit 1
  }
  [ -n "$VERSION" ] || VERSION=$(gh release view --repo "$REPO" --json tagName -q .tagName)
  BINARY="pool-controller-full-${VERSION}.bin"
  echo "Downloading $BINARY …"
  gh release download "$VERSION" --repo "$REPO" --pattern "$BINARY" --dir "$OUT"
fi

cp "$SRC/index.html" "$OUT/index.html"
sed -e "s|__VERSION__|${VERSION}|g" \
    -e "s|__BINARY__|${BINARY}|g" \
    "$SRC/manifest.template.json" > "$OUT/manifest.json"

echo
echo "Serving $VERSION at http://localhost:${PORT}/  (Ctrl-C to stop)"
echo "Use Chrome or Edge — Web Serial is required."
echo
exec python3 -m http.server "$PORT" --directory "$OUT" --bind 127.0.0.1
