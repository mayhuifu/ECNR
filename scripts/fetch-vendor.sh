#!/usr/bin/env bash
# fetch-vendor.sh — re-fetch vendored open-source dependencies pinned by vendor/MANIFEST.tsv
#
# Usage:
#   scripts/fetch-vendor.sh                # fetch all (required + optional)
#   scripts/fetch-vendor.sh required       # baseline tier only (build deps)
#   scripts/fetch-vendor.sh -- <name> ...  # explicit list
#
# Idempotent: skips repos already present at the pinned commit.

set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
MANIFEST="$ROOT/vendor/MANIFEST.tsv"
VENDOR="$ROOT/vendor"

if [[ ! -f "$MANIFEST" ]]; then
  echo "manifest not found: $MANIFEST" >&2
  exit 1
fi

mode="${1:-all}"
shift || true
explicit=()
if [[ "$mode" == "--" ]]; then
  explicit=("$@")
  mode="explicit"
fi

while IFS=$'\t' read -r name url commit tier required; do
  [[ -z "${name:-}" || "${name:0:1}" == "#" ]] && continue

  case "$mode" in
    all) ;;
    required) [[ "$required" == "required" ]] || continue ;;
    explicit)
      keep=0
      for n in "${explicit[@]}"; do [[ "$n" == "$name" ]] && keep=1; done
      [[ $keep == 1 ]] || continue
      ;;
    *) echo "unknown mode: $mode" >&2; exit 2 ;;
  esac

  dest="$VENDOR/$name"
  if [[ -d "$dest/.git" ]]; then
    have=$(git -C "$dest" rev-parse HEAD)
    if [[ "$have" == "$commit" ]]; then
      echo "  ok   $name @ $commit"
      continue
    fi
    echo "  upd  $name : $have -> $commit"
    git -C "$dest" fetch --depth 1 origin "$commit" 2>/dev/null || git -C "$dest" fetch origin
    git -C "$dest" checkout -q "$commit"
  else
    echo "  new  $name @ $commit  ($url)"
    rm -rf "$dest"
    git clone --depth 1 "$url" "$dest"
    git -C "$dest" fetch --depth 1 origin "$commit" 2>/dev/null || git -C "$dest" fetch origin
    git -C "$dest" checkout -q "$commit"
  fi
done < "$MANIFEST"

echo "done."
