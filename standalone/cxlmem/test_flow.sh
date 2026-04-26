#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
cd "$SCRIPT_DIR"

assert_contains() {
  local haystack="$1"
  local needle="$2"
  if ! grep -q "$needle" <<<"$haystack"; then
    echo "[FAIL] expected output to contain: $needle" >&2
    exit 1
  fi
}

run_and_check() {
  local label="$1"; shift
  echo "[INFO] running case: $label"
  local out
  out=$(./cxl_mem_sim "$@")
  echo "$out"

  assert_contains "$out" "completed_requests="
  assert_contains "$out" "total_m2s_flits="
  assert_contains "$out" "total_s2m_flits="
  assert_contains "$out" "throughput_reqs_per_cycle="

  local completed
  completed=$(awk -F= '/^completed_requests=/{print $2}' <<<"$out")
  if [[ -z "$completed" || "$completed" -le 0 ]]; then
    echo "[FAIL] completed_requests must be > 0" >&2
    exit 1
  fi

  local m2s
  m2s=$(awk -F= '/^total_m2s_flits=/{print $2}' <<<"$out")
  local s2m
  s2m=$(awk -F= '/^total_s2m_flits=/{print $2}' <<<"$out")
  if [[ -z "$m2s" || -z "$s2m" || "$m2s" -le 0 || "$s2m" -le 0 ]]; then
    echo "[FAIL] total flits must be > 0" >&2
    exit 1
  fi
}

echo "[INFO] building cxl_mem_sim"
make clean
make

COMMON_ARGS=(--num-reqs 120 --read-percent 60 --max-outstanding 48 --base-m2s-write-slots 4 --base-s2m-read-rsp-slots 4)

run_and_check "baseline" "${COMMON_ARGS[@]}" --extra-m2s-write 0 --extra-s2m-read-rsp 0
run_and_check "m2s_write_plus1" "${COMMON_ARGS[@]}" --extra-m2s-write 1 --extra-s2m-read-rsp 0
run_and_check "s2m_read_rsp_plus1" "${COMMON_ARGS[@]}" --extra-m2s-write 0 --extra-s2m-read-rsp 1
run_and_check "both_plus1" "${COMMON_ARGS[@]}" --extra-m2s-write 1 --extra-s2m-read-rsp 1

echo "[PASS] test flow completed"
