#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
cd "$SCRIPT_DIR"

make -s

for rp in 25 50 75; do
  echo "read_percent=${rp}"
  ./cxl_mem_sim --read-percent "$rp"
  echo
done
