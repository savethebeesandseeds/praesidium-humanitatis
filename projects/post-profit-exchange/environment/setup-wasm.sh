#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Dependency installation only; no container lifecycle or application tasks.
set -euo pipefail
[[ $# == 0 ]] || { echo 'setup-wasm.sh accepts no arguments' >&2; exit 2; }
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
sdk="$root/.build/deps/emsdk-4.0.15"
archive="$root/.temp/wasm-toolchain/emsdk-4.0.15.tar.gz"
expected=35be7626493e3bd22860ee2177147f9bca3b6ff871edeab27c5b061a9ed9d23d
source /etc/os-release
[[ "$ID" == debian && "$VERSION_ID" == 12 ]] || { echo 'Requires Debian 12' >&2; exit 2; }
[[ $(dpkg --print-architecture) == amd64 ]] || { echo 'Requires Linux amd64' >&2; exit 2; }
if [[ $(dpkg-query -W -f='${Version}' python3 2>/dev/null || true) != 3.11.2-1+b1 ]]; then
  apt-get update
  DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends python3=3.11.2-1+b1
fi
mkdir -p "$(dirname "$archive")" "$(dirname "$sdk")"
if [[ ! -f "$archive" ]]; then
  python3 - "$archive" <<'PY'
import sys, urllib.request
urllib.request.urlretrieve('https://github.com/emscripten-core/emsdk/archive/refs/tags/4.0.15.tar.gz', sys.argv[1])
PY
fi
printf '%s  %s\n' "$expected" "$archive" | sha256sum -c -
if [[ ! -d "$sdk" ]]; then
  mkdir "$sdk"
  tar -xzf "$archive" --strip-components=1 -C "$sdk"
fi
[[ -f "$sdk/emsdk" ]] || { echo 'Incomplete SDK staging; preserved for inspection.' >&2; exit 1; }
"$sdk/emsdk" install 4.0.15
"$sdk/emsdk" activate 4.0.15
mkdir -p "$root/.build/post-profit-exchange/environment"
dpkg-query -W > "$root/.build/post-profit-exchange/environment/debian-packages.txt"
"$sdk/upstream/emscripten/em++" --version
