# vendor/

Vendored open-source dependencies. **Not** tracked in git — fetched on demand by `scripts/fetch-vendor.sh` from the upstream URLs pinned in [`MANIFEST.tsv`](MANIFEST.tsv).

This keeps the repo small and provenance reproducible: anyone with a fresh clone runs the fetch script and ends up with the exact commits Phase 0 was built against.

## First-time setup

```sh
# fetch only the build-required baseline (~5 MB)
scripts/fetch-vendor.sh required

# or fetch everything including research code (~450 MB; large pretrained models)
scripts/fetch-vendor.sh
```

## Layout after fetch

```
vendor/
├── webrtc-audio-processing/   # baseline — primary AEC3 + NS + AGC backbone
├── rnnoise/                   # baseline — lightweight NS post-filter
├── speexdsp/                  # baseline — secondary AEC + resampler
├── deepfilternet/             # research — 48 kHz full-band NS option
├── dtln-aec/                  # research — neural AEC reference
├── nkf-aec/                   # research — 5.3K-param Kalman-NN hybrid
└── athena-signal/             # research — multi-mic + RES reference
```

## Local modifications

Each vendored repo is a plain git clone of the upstream. To make local changes, work on a branch in the vendored clone, push to your own fork on GitHub/GitLab, then update `MANIFEST.tsv` to point at your fork URL + commit SHA. Do not commit modifications inside `vendor/<repo>/` to the parent repo — they would be lost on the next fetch.

## License notes

`MANIFEST.tsv` does not include license fields — see [PROJECT.md](../PROJECT.md) for the per-repo license summary. Anything marked "verify" must be re-checked before code derived from it ships in a binary.
