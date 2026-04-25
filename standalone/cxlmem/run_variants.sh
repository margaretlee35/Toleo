#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
cd "$SCRIPT_DIR"

make -s

COMMON_ARGS=(--num-reqs 400 --read-percent 60 --max-outstanding 48 --base-m2s-write-slots 4 --base-s2m-read-rsp-slots 4)

run_case() {
  local label="$1"; shift
  echo "===== ${label} ====="
  ./cxl_mem_sim "${COMMON_ARGS[@]}" "$@"
  echo
}

run_case "baseline" --extra-m2s-write 0 --extra-s2m-read-rsp 0
run_case "m2s_write_plus1" --extra-m2s-write 1 --extra-s2m-read-rsp 0
run_case "s2m_read_rsp_plus1" --extra-m2s-write 0 --extra-s2m-read-rsp 1
run_case "both_plus1" --extra-m2s-write 1 --extra-s2m-read-rsp 1
