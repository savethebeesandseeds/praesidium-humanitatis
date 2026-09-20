#!/usr/bin/env bash
# Project operations only; never creates, starts or stops a container.
set -euo pipefail
project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
cd -- "$project_root"
action="${1:-help}"
if [[ $# -gt 0 ]]; then shift; fi
case "$action" in
  configure|build|test|test-cuda|demo) ;;
  help)
    echo 'Usage: bash tools/temporal-fusion-transformer/tasks.sh {configure|build|test|test-cuda|demo [arguments...]}'
    echo 'PRAESIDIUM_HUMANITATIS_BUILD_JOBS: positive build concurrency (default: 2).'
    exit 0 ;;
  *) echo "Unknown task: $action" >&2; exit 1 ;;
esac
if [[ "$action" != demo && $# -ne 0 ]]; then
  echo 'Only demo accepts additional arguments.' >&2; exit 1
fi
[[ -f /etc/profile.d/praesidium-humanitatis.sh ]] || {
  echo 'Run bash tools/temporal-fusion-transformer/environment/setup.sh in the project container first.' >&2; exit 1;
}
source /etc/profile.d/praesidium-humanitatis.sh
jobs="${PRAESIDIUM_HUMANITATIS_BUILD_JOBS:-2}"
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo 'PRAESIDIUM_HUMANITATIS_BUILD_JOBS must be a positive integer.' >&2; exit 1; }
cuda_tests=OFF
[[ "$action" != test-cuda ]] || cuda_tests=ON
cmake -S "$project_root/tools/temporal-fusion-transformer" -B "$project_root/.build/temporal-fusion-transformer" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$project_root/.build/deps/libtorch" \
  -DBUILD_TESTING=ON -DPRAESIDIUM_HUMANITATIS_CUDA_TESTS="$cuda_tests"
[[ "$action" != configure ]] || exit 0
cmake --build .build/temporal-fusion-transformer --parallel "$jobs"
case "$action" in
  test) ctest --test-dir .build/temporal-fusion-transformer --output-on-failure --no-tests=error -R '_cpu$' ;;
  test-cuda) ctest --test-dir .build/temporal-fusion-transformer --output-on-failure --no-tests=error -R '_cuda$' ;;
  demo) exec .build/temporal-fusion-transformer/tft_demo "$@" ;;
esac
