#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
exec bash "$project_root/tools/temporal-fusion-transformer/tasks.sh" "$@"
