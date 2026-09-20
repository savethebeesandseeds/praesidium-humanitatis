#!/usr/bin/env bash
# SPDX-License-Identifier: MIT AND LicenseRef-Praesidium-Humanitatis-Worker-Protection-1.0
# Copyright (c) 2026 Waajacu
# Adapted 2026-09-20 from MIT TFT setup; see NOTICE.md and LICENSE-MIT-TFT-LAUNCHER.
# Dependencies and environment only. Lifecycle: container.ps1; operations: ../tasks.sh.
set -euo pipefail
[[ $# -eq 0 ]] || { echo 'Usage: bash tools/price-optimization/environment/setup.sh (no arguments)' >&2; exit 1; }
environment_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd -- "$environment_dir/../../.." && pwd)"
[[ "$project_root" -ef /workspace/praesidium-humanitatis ]] || {
  echo 'Run setup inside the approved container at /workspace/praesidium-humanitatis.' >&2; exit 1;
}
project_root=/workspace/praesidium-humanitatis
source /etc/os-release
[[ "$ID" == debian && "$VERSION_ID" == 12 && "$(dpkg --print-architecture)" == amd64 ]] || {
  echo 'setup.sh requires Debian 12 amd64.' >&2; exit 1;
}
[[ $EUID -eq 0 ]] || { echo 'Run setup.sh as root inside the container.' >&2; exit 1; }
lock="$environment_dir/dependencies.lock"
[[ -s "$lock" ]] || { echo 'Missing dependency lock.' >&2; exit 1; }
if grep -Ev '^([a-z0-9][a-z0-9+.-]*=[^[:space:]]+|#.*)$' "$lock"; then
  echo 'Invalid dependency lock line; expected package=version or comment.' >&2; exit 1
fi
mapfile -t packages < <(grep -v '^#' "$lock")
[[ ${#packages[@]} -gt 0 ]] || { echo 'Empty dependency lock.' >&2; exit 1; }
[[ "$(printf '%s\n' "${packages[@]}" | cut -d= -f1 | sort | uniq -d | wc -l)" == 0 ]] || {
  echo 'Duplicate package in dependency lock.' >&2; exit 1;
}
export DEBIAN_FRONTEND=noninteractive
apt=(apt-get -o Acquire::Retries=3 -o Acquire::https::Timeout=30 -o Acquire::http::Timeout=30)
"${apt[@]}" update
"${apt[@]}" install -y --no-install-recommends "${packages[@]}"
for pin in "${packages[@]}"; do
  package="${pin%%=*}"
  expected="${pin#*=}"
  installed="$(dpkg-query -W -f='${Version}' "$package")"
  [[ "$installed" == "$expected" ]] || {
    printf 'Package mismatch: %s expected %s, found %s\n' "$package" "$expected" "$installed" >&2
    exit 1
  }
done

# AMPL, the C++ API and solver bundles are staged by the user under their own terms.
# This setup never downloads, activates, purchases or redistributes those products.
cat > /etc/profile.d/praesidium-price-optimization.sh <<'EOF'
export AMPLAPI_ROOT=/workspace/praesidium-humanitatis/.build/deps/amplapi
export PH_AMPL_BINARY_DIR=/workspace/praesidium-humanitatis/.build/deps/ampl
EOF
profile_line='source /etc/profile.d/praesidium-price-optimization.sh'
grep -Fqx "$profile_line" /root/.bashrc || printf '\n%s\n' "$profile_line" >> /root/.bashrc
source /etc/profile.d/praesidium-price-optimization.sh
mkdir -p "$project_root/.build/price-optimization/environment"
dpkg-query -W -f='${Package}=${Version}\n' > "$project_root/.build/price-optimization/environment/debian-packages.txt"
audit="$(dpkg --audit)"
[[ -z "$audit" ]] || { printf '%s\n' "$audit" >&2; exit 1; }
cmake --version
printf 'CPU dependencies ready: GCC %s. AMPL execution requires a separately staged licensed runtime, solver and C++ API.\n' "$(g++ -dumpfullversion)"
