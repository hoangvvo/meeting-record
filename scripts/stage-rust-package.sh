#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$ROOT/rust/native"

case "$DEST" in
  "$ROOT/rust/native") ;;
  *) echo "refusing to replace unexpected path: $DEST" >&2; exit 1 ;;
esac

rm -rf "$DEST"
mkdir -p "$DEST"
cp -R "$ROOT/native/include" "$ROOT/native/macos" "$ROOT/native/windows" "$DEST/"
