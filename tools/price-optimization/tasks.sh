#!/usr/bin/env bash
# SPDX-License-Identifier: MIT AND LicenseRef-Praesidium-Humanitatis-Worker-Protection-1.0
# Copyright (c) 2026 Waajacu
# Adapted 2026-09-20 from MIT TFT tasks; see environment/NOTICE.md.
# Project operations only; never creates, starts or stops a container.
set -euo pipefail
project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
cd -- "$project_root"
action="${1:-help}"
if [[ $# -gt 0 ]]; then shift; fi
case "$action" in
  configure|build|test|demo|test-ampl|demo-ampl) ;;
  help)
    echo 'Usage: bash tools/price-optimization/tasks.sh {configure|build|test|demo|test-ampl|demo-ampl}'
    echo 'PH_PRICE_WITH_AMPL: ON/OFF (default OFF); AMPLAPI_ROOT: staged C++ API directory.'
    echo 'PH_AMPL_BINARY_DIR: licensed AMPL/solver directory; PH_AMPL_SOLVER: highs (only supported driver).'
    echo 'PRAESIDIUM_HUMANITATIS_BUILD_JOBS: positive build concurrency (default 2).'
    exit 0 ;;
  *) echo "Unknown task: $action" >&2; exit 1 ;;
esac
[[ $# -eq 0 ]] || { echo 'Tasks do not accept extra arguments; use the documented environment variables.' >&2; exit 1; }
[[ -f /etc/profile.d/praesidium-price-optimization.sh ]] || {
  echo 'Run bash tools/price-optimization/environment/setup.sh in the optimizer container first.' >&2; exit 1;
}
# Preserve explicit SDK/runtime overrides while loading defaults for docker exec.
amplapi_root="${AMPLAPI_ROOT:-$project_root/.build/deps/amplapi}"
ampl_binary_dir="${PH_AMPL_BINARY_DIR:-$project_root/.build/deps/ampl}"
source /etc/profile.d/praesidium-price-optimization.sh
export AMPLAPI_ROOT="$amplapi_root" PH_AMPL_BINARY_DIR="$ampl_binary_dir"
solver="${PH_AMPL_SOLVER:-highs}"
jobs="${PRAESIDIUM_HUMANITATIS_BUILD_JOBS:-2}"
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo 'PRAESIDIUM_HUMANITATIS_BUILD_JOBS must be a positive integer.' >&2; exit 1; }
with_ampl="${PH_PRICE_WITH_AMPL:-OFF}"
[[ "$with_ampl" == ON || "$with_ampl" == OFF ]] || { echo 'PH_PRICE_WITH_AMPL must be ON or OFF.' >&2; exit 1; }
integration_binary_dir=''
if [[ "$action" == test-ampl || "$action" == demo-ampl ]]; then
  with_ampl=ON
  [[ -x "$ampl_binary_dir/ampl" ]] || { echo "Stage an executable licensed AMPL runtime in $ampl_binary_dir first." >&2; exit 1; }
  [[ "$solver" == highs ]] || { echo 'Only the HiGHS driver (PH_AMPL_SOLVER=highs) is supported in this version.' >&2; exit 1; }
  if [[ "$action" == test-ampl ]]; then integration_binary_dir="$ampl_binary_dir"; fi
fi
if [[ "$with_ampl" == ON ]]; then
  [[ -f "$amplapi_root/include/ampl/ampl.h" ]] || {
    echo "Stage the AMPL C++ API include/ampl/ampl.h and matching library in $amplapi_root first." >&2; exit 1;
  }
fi
cmake -S "$project_root/tools/price-optimization" -B "$project_root/.build/price-optimization" \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DPH_PRICE_WITH_AMPL="$with_ampl" -DAMPLAPI_ROOT="$amplapi_root" \
  -DPH_AMPL_BINARY_DIR="$integration_binary_dir" -DPH_AMPL_SOLVER="$solver"
[[ "$action" != configure ]] || exit 0
cmake --build "$project_root/.build/price-optimization" --parallel "$jobs"
case "$action" in
  test|test-ampl) ctest --test-dir "$project_root/.build/price-optimization" --output-on-failure --no-tests=error ;;
  demo) exec "$project_root/.build/price-optimization/price_guardrail_demo" ;;
  demo-ampl) exec "$project_root/.build/price-optimization/price_ampl_demo" "$ampl_binary_dir" "$solver" ;;
esac
