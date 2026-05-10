#!/usr/bin/env bash
# fetch-noise.sh — fetch + verify real-recording noise corpus pinned by
# reference/noise/MANIFEST.tsv. Companion to fetch-vendor.sh.
#
# Usage:
#   scripts/fetch-noise.sh           # fetch all auto entries; verify all entries
#   scripts/fetch-noise.sh --check   # verify only; no downloads
#
# Manifest columns (tab-separated): local_path, source_url, license, attribution,
# sha256, fetch_method (auto | manual).
#
# Behaviour:
#   - For `auto` entries: download if missing or SHA256 mismatch. Two-step
#     resolve (HEAD to follow archive.org's redirect, then GET the CDN URL) —
#     archive.org's bare /download/ URL sometimes 500s on streamed redirect
#     follow in restricted shells.
#   - For `manual` entries: never download. If the file is missing, print a
#     "go to URL X, save as Y" instruction. If present but SHA256 mismatches,
#     fail loudly so the user can swap or re-pin.
#
# Files in `reference/noise/` are git-ignored; only MANIFEST.tsv + README.md
# are committed. Re-running this script is idempotent: present files at correct
# SHA256 print `ok` and do nothing.

set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
MANIFEST="$ROOT/reference/noise/MANIFEST.tsv"

CHECK_ONLY=0
if [[ "${1:-}" == "--check" ]]; then
  CHECK_ONLY=1
fi

if [[ ! -f "$MANIFEST" ]]; then
  echo "manifest not found: $MANIFEST" >&2
  exit 1
fi

# Cross-platform sha256 (macOS: shasum -a 256; Linux: sha256sum).
sha256_of() {
  local path="$1"
  if command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$path" | awk '{print $1}'
  elif command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$path" | awk '{print $1}'
  else
    echo "no sha256 tool found (need shasum or sha256sum)" >&2
    return 1
  fi
}

# archive.org's /download/ URL 302-redirects to a CDN host. Some shells can't
# follow that (curl -L returns 500 mid-stream). This resolves the Location
# header explicitly so the GET hits the CDN directly.
resolve_redirect() {
  local url="$1"
  local loc
  loc="$(curl -fsSI --max-time 30 "$url" 2>/dev/null \
    | grep -i '^location:' \
    | tr -d '\r' \
    | awk '{print $2}' \
    | tail -n1)"
  if [[ -n "$loc" ]]; then
    printf '%s' "$loc"
  else
    printf '%s' "$url"
  fi
}

n_ok=0
n_fetched=0
n_missing=0
n_mismatch=0

while IFS=$'\t' read -r local_path source_url license attribution expected_sha fetch_method; do
  # Skip comments + blank lines.
  case "$local_path" in
    '' | '#'*) continue ;;
  esac

  abs_path="$ROOT/$local_path"
  short_name="$(basename "$local_path")"

  if [[ -f "$abs_path" ]]; then
    actual_sha="$(sha256_of "$abs_path")"
    if [[ "$actual_sha" == "$expected_sha" ]]; then
      printf '  ok      %-22s [%s]\n' "$short_name" "$license"
      n_ok=$((n_ok + 1))
      continue
    else
      printf '  MISMATCH %-21s expected %s got %s\n' \
        "$short_name" "${expected_sha:0:12}..." "${actual_sha:0:12}..." >&2
      n_mismatch=$((n_mismatch + 1))
      continue
    fi
  fi

  # File is missing. Branch on fetch_method.
  case "$fetch_method" in
    auto)
      if (( CHECK_ONLY == 1 )); then
        printf '  MISSING %-22s (--check: would auto-fetch from %s)\n' \
          "$short_name" "$source_url"
        n_missing=$((n_missing + 1))
        continue
      fi
      printf '  fetch   %-22s [%s]\n' "$short_name" "$license"
      mkdir -p "$(dirname "$abs_path")"
      resolved="$(resolve_redirect "$source_url")"
      if ! curl -fsSL --max-time 120 -o "$abs_path" "$resolved"; then
        printf '          DOWNLOAD FAILED for %s\n' "$source_url" >&2
        rm -f "$abs_path"
        n_missing=$((n_missing + 1))
        continue
      fi
      actual_sha="$(sha256_of "$abs_path")"
      if [[ "$actual_sha" != "$expected_sha" ]]; then
        printf '          SHA MISMATCH after fetch: expected %s got %s\n' \
          "${expected_sha:0:12}..." "${actual_sha:0:12}..." >&2
        n_mismatch=$((n_mismatch + 1))
        continue
      fi
      n_fetched=$((n_fetched + 1))
      ;;
    manual)
      printf '  MANUAL  %-22s — save from %s\n' "$short_name" "$source_url"
      printf '          to %s  (CC0; no attribution required)\n' "$abs_path"
      n_missing=$((n_missing + 1))
      ;;
    *)
      echo "unknown fetch_method '$fetch_method' for $local_path" >&2
      exit 1
      ;;
  esac
done < "$MANIFEST"

echo ""
printf 'summary: %d ok, %d fetched, %d missing, %d mismatch\n' \
  "$n_ok" "$n_fetched" "$n_missing" "$n_mismatch"

# Exit non-zero if anything is wrong, so CI / pre-commit can gate on it.
if (( n_mismatch > 0 )) || (( n_missing > 0 && CHECK_ONLY == 1 )); then
  exit 1
fi
exit 0
