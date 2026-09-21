#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
project="$root/projects/post-profit-exchange"
build="$root/.build/post-profit-exchange/wasm"
sdk="${EXCHANGE_EMSDK:-$root/.build/deps/emsdk-4.0.15}"
[[ -f "$sdk/emsdk_env.sh" ]] || { echo 'Run environment/setup-wasm.sh first.' >&2; exit 1; }
source "$sdk/emsdk_env.sh" >/dev/null 2>&1
em++ --version | head -1 | grep -F '4.0.15' >/dev/null || { echo 'Requires pinned Emscripten 4.0.15.' >&2; exit 1; }
mkdir -p "$build"
python3 "$project/simulation/embed-config.py" "$build/generated/default_config.hpp"
em++ -std=c++17 -O2 -fexceptions -Wall -Wextra -Wpedantic \
  -I"$root/tools/price-optimization/include" \
  -I"$root/tools/exponential-smoothing/include" \
  -I"$build/generated" \
  -isystem "$root/tools/price-optimization/third_party" \
  "$project/simulation/simulation.cpp" \
  "$project/simulation/models.cpp" \
  "$root/tools/exponential-smoothing/src/ewma.cpp" \
  "$root/tools/price-optimization/src/engine.cpp" \
  "$root/tools/price-optimization/src/enumeration.cpp" \
  --no-entry -sMODULARIZE=1 -sEXPORT_NAME=createExchangeModule \
  -sSINGLE_FILE=1 -sENVIRONMENT=web,worker,node -sALLOW_MEMORY_GROWTH=1 \
  -sMAXIMUM_MEMORY=268435456 -sSTACK_SIZE=1048576 -sFILESYSTEM=0 \
  -sDYNAMIC_EXECUTION=0 -sDISABLE_EXCEPTION_CATCHING=0 -sEMIT_EMSCRIPTEN_LICENSE=1 \
  -sEXPORTED_FUNCTIONS='["_exchange_run","_malloc","_free"]' \
  -sEXPORTED_RUNTIME_METHODS='["ccall"]' -o "$build/exchange-runtime.js"
python3 "$project/package-html.py" "$build/exchange-runtime.js" "$sdk"
