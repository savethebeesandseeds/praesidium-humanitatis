#!/usr/bin/env bash
# Dependencies and environment only. Container lifecycle: container.ps1.
# Build, test and execution: ../tasks.sh.
set -euo pipefail
[[ $# -eq 0 ]] || { echo 'Usage: bash tools/temporal-fusion-transformer/environment/setup.sh (no arguments)' >&2; exit 1; }
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
torch_root="$project_root/.build/deps/libtorch"
[[ -f "$torch_root/include/torch/csrc/api/include/torch/torch.h" &&
   -f "$torch_root/build-version" && -f "$torch_root/share/cmake/Torch/TorchConfig.cmake" ]] || {
  echo 'Stage LibTorch in .build/deps/libtorch first; see docs/environment.md.' >&2; exit 1;
}
[[ "$(cat "$torch_root/build-version")" == '2.6.0+cu124' ]] || {
  echo 'Expected LibTorch 2.6.0+cu124 (Linux C++11 ABI) bundle.' >&2; exit 1;
}
grep -q -- '-D_GLIBCXX_USE_CXX11_ABI=1' "$torch_root/share/cmake/Torch/TorchConfig.cmake" || {
  echo 'LibTorch must use the C++11 ABI.' >&2; exit 1;
}
for library in libtorch.so libtorch_cpu.so libtorch_cuda.so; do
  [[ -f "$torch_root/lib/$library" ]] || { echo "Missing $library in staged bundle." >&2; exit 1; }
done
lock="$environment_dir/dependencies.lock"
[[ -s "$lock" ]] || { echo 'Missing dependency lock.' >&2; exit 1; }
if grep -Ev '^[a-z0-9][a-z0-9+.-]*=[^[:space:]]+$' "$lock"; then
  echo 'Invalid dependency lock line; expected package=version.' >&2; exit 1
fi
[[ "$(cut -d= -f1 "$lock" | sort | uniq -d | wc -l)" == 0 ]] || {
  echo 'Duplicate package in dependency lock.' >&2; exit 1;
}
export DEBIAN_FRONTEND=noninteractive
apt=(apt-get -o Acquire::Retries=3 -o Acquire::https::Timeout=30 -o Acquire::http::Timeout=30)
mkdir -p "$project_root/.temp/environment" "$project_root/.build/environment"
"${apt[@]}" update
mapfile -t bootstrap < <(grep -E '^(ca-certificates|curl)=' "$lock")
[[ ${#bootstrap[@]} -eq 2 ]] || { echo 'Missing bootstrap package pins.' >&2; exit 1; }
"${apt[@]}" install -y --no-install-recommends "${bootstrap[@]}"

# The reference keyring/repository is retained; check the downloaded package.
keyring="$project_root/.temp/environment/cuda-keyring_1.1-1_all.deb"
if [[ "$(dpkg-query -W -f='${Version}' cuda-keyring 2>/dev/null || true)" != '1.1-1' ]]; then
  curl -fsSL -o "$keyring" https://developer.download.nvidia.com/compute/cuda/repos/debian12/x86_64/cuda-keyring_1.1-1_all.deb
  printf '%s  %s\n' e7f219eab6fe4819cdb5c15b98233dc3420302d9c00883219cd3d896857cf48d "$keyring" | sha256sum -c -
  dpkg -i "$keyring"
fi
"${apt[@]}" update
mapfile -t packages < "$lock"
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

sed -i 's/^[[:space:]]*#[[:space:]]*en_US.UTF-8 UTF-8/en_US.UTF-8 UTF-8/' /etc/locale.gen
locale-gen en_US.UTF-8
update-locale --reset LANG=en_US.UTF-8

# Keep bundled cuDNN components together; never add CUDA stubs to this path.
cat > /etc/profile.d/praesidium-humanitatis.sh <<'EOF'
export CUDA_VERSION=12.4
export CUDNN_VERSION=9
case ":$PATH:" in
  *:/usr/local/cuda-12.4/bin:*) ;;
  *) export PATH="/usr/local/cuda-12.4/bin:$PATH" ;;
esac
case "${LD_LIBRARY_PATH:-}" in
  /workspace/praesidium-humanitatis/.build/deps/libtorch/lib:/usr/local/cuda-12.4/lib64*) ;;
  *) export LD_LIBRARY_PATH="/workspace/praesidium-humanitatis/.build/deps/libtorch/lib:/usr/local/cuda-12.4/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" ;;
esac
EOF
profile_line='source /etc/profile.d/praesidium-humanitatis.sh'
grep -Fqx "$profile_line" /root/.bashrc || printf '\n%s\n' "$profile_line" >> /root/.bashrc
source /etc/profile.d/praesidium-humanitatis.sh
dpkg-query -W -f='${Package}=${Version}\n' > "$project_root/.build/environment/debian-packages.txt"
audit="$(dpkg --audit)"
[[ -z "$audit" ]] || { printf '%s\n' "$audit" >&2; exit 1; }
for library in libtorch.so libtorch_cpu.so libtorch_cuda.so; do
  dependencies="$(ldd "$torch_root/lib/$library")"
  if grep -q 'not found' <<< "$dependencies"; then
    printf '%s\n' "$dependencies" >&2
    echo 'Missing LibTorch runtime dependency; environment preserved for inspection.' >&2
    exit 1
  fi
done
nvcc --version
cmake --version
dpkg-query -W cuda-toolkit-12-4 cudnn9-cuda-12
nvidia-smi --query-gpu=name,driver_version,memory.total --format=csv,noheader
printf 'Dependencies ready: GCC %s; LibTorch %s\n' "$(g++ -dumpfullversion)" "$(cat "$torch_root/build-version")"
