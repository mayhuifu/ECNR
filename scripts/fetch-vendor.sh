#!/usr/bin/env bash
# fetch-vendor.sh — re-fetch vendored open-source dependencies pinned by vendor/MANIFEST.tsv
#
# Usage:
#   scripts/fetch-vendor.sh                # fetch all (required + optional)
#   scripts/fetch-vendor.sh required       # baseline tier only (build deps)
#   scripts/fetch-vendor.sh -- <name> ...  # explicit list
#
# Idempotent: skips repos already present at the pinned commit.
#
# Per-vendor post-fetch hooks live in `post_fetch_<name>` shell functions
# below. They run after every checkout and must themselves be idempotent
# (re-running with the artifact already present is a no-op). Currently only
# rnnoise needs one — to download model weights not committed upstream.

set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
MANIFEST="$ROOT/vendor/MANIFEST.tsv"
VENDOR="$ROOT/vendor"

if [[ ! -f "$MANIFEST" ]]; then
  echo "manifest not found: $MANIFEST" >&2
  exit 1
fi

# Per-vendor post-fetch hooks. Idempotent: a hook is invoked after every
# successful checkout (including the "already at pinned commit" path) and is
# expected to no-op when its outputs already exist.
post_fetch_rnnoise() {
  local dest="$1"
  # rnnoise_data.{c,h} are extracted from a tarball downloaded by upstream's
  # download_model.sh; they're not committed in the rnnoise git tree but are
  # required for our inline build of `ecnr_rnnoise`.
  if [[ -f "$dest/src/rnnoise_data.c" && -f "$dest/src/rnnoise_data.h" ]]; then
    echo "       rnnoise model already present"
    return 0
  fi
  echo "       fetching rnnoise model weights"
  # Upstream script uses wget; on macOS we may not have it, so fall back to
  # curl while still validating against the pinned sha256 in `model_version`.
  ( cd "$dest" \
    && hash=$(cat model_version) \
    && model="rnnoise_data-$hash.tar.gz" \
    && if [[ ! -f "$model" ]]; then
         if command -v wget >/dev/null 2>&1; then
           wget -q "https://media.xiph.org/rnnoise/models/$model"
         else
           curl -fsSL -o "$model" "https://media.xiph.org/rnnoise/models/$model"
         fi
       fi \
    && if command -v sha256sum >/dev/null 2>&1; then
         got=$(sha256sum "$model" | awk '{print $1}')
       else
         got=$(shasum -a 256 "$model" | awk '{print $1}')
       fi \
    && [[ "$got" == "$hash" ]] || { echo "rnnoise model checksum mismatch" >&2; exit 1; } \
    && tar xomf "$model" )
}

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
    else
      echo "  upd  $name : $have -> $commit"
      git -C "$dest" fetch --depth 1 origin "$commit" 2>/dev/null || git -C "$dest" fetch origin
      git -C "$dest" checkout -q "$commit"
    fi
  else
    echo "  new  $name @ $commit  ($url)"
    rm -rf "$dest"
    git clone --depth 1 "$url" "$dest"
    git -C "$dest" fetch --depth 1 origin "$commit" 2>/dev/null || git -C "$dest" fetch origin
    git -C "$dest" checkout -q "$commit"
  fi

  # Run a per-vendor post-fetch hook if one is defined. Hooks are idempotent
  # so we can call them on every pass (including "already at pinned commit").
  if declare -f "post_fetch_$name" >/dev/null 2>&1; then
    "post_fetch_$name" "$dest"
  fi
done < "$MANIFEST"

echo "done."
