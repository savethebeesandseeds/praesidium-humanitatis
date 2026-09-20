#!/usr/bin/env bash
# Isolated legacy reference dependencies only; no container lifecycle or training.
set -euo pipefail
[[ $# -eq 0 ]] || { echo 'Usage: bash tools/temporal-fusion-transformer/environment/setup-reference.sh'; exit 1; }
project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)"
[[ "$project_root" -ef /workspace/praesidium-humanitatis ]] || {
  echo 'Use the existing approved project container.' >&2; exit 1;
}
runtime="$project_root/.build/reference-runtime"
download_dir="$project_root/.temp/reference-runtime"
mkdir -p "$download_dir" "$project_root/.build/verification/replication"
installer="$download_dir/Miniconda3-py37_23.1.0-1-Linux-x86_64.sh"
installer_sha=fc96109ea96493e31f70abbc5cae58e80634480c0686ab46924549ac41176812
if [[ ! -f "$installer" ]]; then
  curl --fail --location --retry 3 --connect-timeout 30 \
    https://repo.anaconda.com/miniconda/Miniconda3-py37_23.1.0-1-Linux-x86_64.sh \
    -o "$installer.partial"
  printf '%s  %s\n' "$installer_sha" "$installer.partial" | sha256sum -c -
  mv -- "$installer.partial" "$installer"
fi
printf '%s  %s\n' "$installer_sha" "$installer" | sha256sum -c -
if [[ ! -e "$runtime" ]]; then
  bash "$installer" -b -p "$runtime"
fi
[[ -x "$runtime/bin/python" ]] || { echo 'Incomplete runtime preserved; inspect before repair.' >&2; exit 1; }
"$runtime/bin/python" -c 'import sys; assert sys.version_info[:2] == (3, 7), sys.version'
# No conda channel operations, shell initialization, or system Python changes.
PIP_DISABLE_PIP_VERSION_CHECK=1 "$runtime/bin/python" -m pip install \
  --no-input --progress-bar off --cache-dir "$download_dir/pip-cache" \
  -r "$project_root/tools/temporal-fusion-transformer/environment/reference-requirements.txt"
"$runtime/bin/python" -m pip check
"$runtime/bin/python" -m pip freeze --all \
  > "$project_root/.build/verification/replication/reference-freeze.txt"
TF_CPP_MIN_LOG_LEVEL=2 CUDA_VISIBLE_DEVICES='' "$runtime/bin/python" - <<'PY'
import sys
import numpy as np
import tensorflow as tf
assert tf.__version__ == '1.15.5', tf.__version__
with tf.Session(config=tf.ConfigProto(intra_op_parallelism_threads=1, inter_op_parallelism_threads=1)) as session:
    result = session.run(tf.reduce_sum(tf.constant([1.0, 2.0])))
assert result == 3.0
print('Reference runtime verified:', sys.version.split()[0], 'TensorFlow', tf.__version__, 'NumPy', np.__version__)
PY
