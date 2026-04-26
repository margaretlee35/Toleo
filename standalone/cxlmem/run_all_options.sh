#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
cd "$SCRIPT_DIR"

STAMP=${1:-$(date +%Y%m%d_%H%M%S)}
OUT_DIR="$SCRIPT_DIR/results/$STAMP"
RAW_DIR="$OUT_DIR/raw"
mkdir -p "$RAW_DIR"

make -s

BASE_ARGS=(--num-reqs 300 --read-percent 60 --max-outstanding 48 --base-m2s-write-slots 4 --base-s2m-read-rsp-slots 4)

run_case() {
  local name="$1"; shift
  echo "[RUN] $name"
  ./cxl_mem_sim "${BASE_ARGS[@]}" "$@" | tee "$RAW_DIR/${name}.log" >/dev/null
}

# Main four perturbation options.
run_case baseline --extra-m2s-write 0 --extra-s2m-read-rsp 0
run_case m2s_plus1 --extra-m2s-write 1 --extra-s2m-read-rsp 0
run_case s2m_plus1 --extra-m2s-write 0 --extra-s2m-read-rsp 1
run_case both_plus1 --extra-m2s-write 1 --extra-s2m-read-rsp 1

# "Each option" quick sensitivity sweeps (kept short).
for mo in 8 16 32 64; do
  run_case max_outstanding_${mo} --max-outstanding "$mo" --extra-m2s-write 1 --extra-s2m-read-rsp 1
 done

for ser in 2 4 8; do
  run_case serdes_${ser} --serdes-time-per-flit "$ser" --extra-m2s-write 1 --extra-s2m-read-rsp 1
 done

for rp in 20 50 80; do
  run_case readpct_${rp} --read-percent "$rp" --extra-m2s-write 1 --extra-s2m-read-rsp 1
 done

python3 "$SCRIPT_DIR/parse_results.py" --input-dir "$RAW_DIR" --output-csv "$OUT_DIR/summary.csv" --emit-simout-dir "$OUT_DIR/simout_compat"
python3 "$SCRIPT_DIR/plot_results.py" --csv "$OUT_DIR/summary.csv" --out-dir "$OUT_DIR/plots"

echo "[DONE] results at: $OUT_DIR"
echo "  - raw logs:      $RAW_DIR"
echo "  - summary csv:   $OUT_DIR/summary.csv"
echo "  - simout compat: $OUT_DIR/simout_compat"
echo "  - plots:         $OUT_DIR/plots"
