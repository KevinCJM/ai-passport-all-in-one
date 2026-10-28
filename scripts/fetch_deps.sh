#!/usr/bin/env bash
# Downloads the Espressif registry components the firmware builds against into deps/, at the exact versions
# 0.5.12 was built with, and checks each archive's SHA-256.
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
BASE=https://components-file.espressif.com/components
mkdir -p "$ROOT/deps"
while read -r dir path sha; do
  [[ -d "$ROOT/deps/$dir" ]] && continue
  zip="$ROOT/deps/$dir.zip"
  echo "fetching $path"
  curl -fL --retry 5 --retry-all-errors -sS -o "$zip" "$BASE/$path"
  echo "$sha  $zip" | shasum -a 256 -c --status || { echo "checksum mismatch: $path" >&2; rm -f "$zip"; exit 1; }
  rm -rf "$ROOT/deps/$dir.tmp" && mkdir "$ROOT/deps/$dir.tmp" && unzip -q "$zip" -d "$ROOT/deps/$dir.tmp"
  mv "$ROOT/deps/$dir.tmp" "$ROOT/deps/$dir" && rm "$zip"
done <<'EOF'
lvgl__lvgl lvgl/lvgl/9.5.0/lvgl__lvgl-v9.5.0.zip 95bd2126bdcc8ccd3407eff7c9e76687e41a8a4312bac1538b3d644b461c07fb
espressif__cjson espressif/cjson/1.7.19~2/espressif__cjson-v1.7.19_2.zip 2e13f18fa612d85eeec4001dee87b246beac64cf019bc28d4c4e2e1910adc7f1
espressif__esp_audio_codec espressif/esp_audio_codec/2.5.0/espressif__esp_audio_codec-v2.5.0.zip a3fd27962c8e83fc35fc084d2195c5945d276896d15c2ac4dad0fe084b87e1a8
espressif__esp_codec_dev espressif/esp_codec_dev/1.6.2/espressif__esp_codec_dev-v1.6.2.zip 50b5d2b7687fd7f6e93fda110aecf00cfe12e2bd34d77b90a01efb6dd904d88d
espressif__esp-dsp espressif/esp-dsp/1.8.0/espressif__esp-dsp-v1.8.0.zip 2ff8097d87a08f31a7252b5840088ea35a7e155a6c8afbe3a8184ae6fda31bfa
EOF
