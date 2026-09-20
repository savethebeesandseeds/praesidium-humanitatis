#!/usr/bin/env bash
# Verification operations only; use the already provisioned project container.
set -euo pipefail
[[ $# -eq 1 ]] || { echo 'Usage: bash tools/temporal-fusion-transformer/replication/verify.sh NEW_OUTPUT_DIRECTORY' >&2; exit 1; }
project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd -- "$project_root"
source /etc/profile.d/praesidium-humanitatis.sh
reference_python="$project_root/.build/reference-runtime/bin/python"
[[ -x "$reference_python" ]] || { echo 'Provision setup-reference.sh first.' >&2; exit 1; }
output="$1"
[[ ! -e "$output" ]] || { echo 'Output already exists; preserve it and choose a new path.' >&2; exit 1; }
mkdir -p -- "$(dirname -- "$output")"
mkdir -- "$output"
sha256sum tools/temporal-fusion-transformer/replication/*.{py,h,json,sh} tools/temporal-fusion-transformer/src/*.cpp tools/temporal-fusion-transformer/include/praesidium-humanitatis/*.h \
  tools/temporal-fusion-transformer/examples/*.cpp tools/temporal-fusion-transformer/CMakeLists.txt tools/temporal-fusion-transformer/environment/reference-requirements.txt \
  > "$output/source-sha256.txt"
"$reference_python" -m pip freeze --all > "$output/reference-freeze.txt"
cmake -S tools/temporal-fusion-transformer -B .build/temporal-fusion-transformer -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$project_root/.build/deps/libtorch" \
  -DBUILD_TESTING=ON -DPRAESIDIUM_HUMANITATIS_CUDA_TESTS=ON \
  > "$output/configure.log" 2>&1
cmake --build .build/temporal-fusion-transformer --target tft_compare_reference compatibility_test checkpoint_test --parallel 2 \
  > "$output/build.log" 2>&1
ctest --test-dir .build/temporal-fusion-transformer --verbose --no-tests=error -R '^(compatibility|checkpoint)_' \
  > "$output/compatibility.log" 2>&1
"$reference_python" tools/temporal-fusion-transformer/replication/test_electricity.py --reference-root .build/reference/tft \
  > "$output/data-fixtures.log" 2>&1
for specification in mixed:1729 mixed:2718 mixed:31415 electricity:1729; do
  preset="${specification%%:*}"
  seed="${specification##*:}"
  fixture="$output/$preset-$seed"
  "$reference_python" tools/temporal-fusion-transformer/replication/export_reference.py --reference-root .build/reference/tft \
    --output "$fixture" --preset "$preset" --seed "$seed" > "$fixture-export.log" 2>&1
  for device in cpu cuda; do
    .build/temporal-fusion-transformer/tft_compare_reference "$fixture" --device "$device" \
      > "$fixture-$device.log" 2>&1
    cat "$fixture-$device.log"
  done
done
echo 'PASS: original-reference numerical fixtures and protocol checks. Full benchmark replication remains separate.'
