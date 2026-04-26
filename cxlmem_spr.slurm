#!/usr/bin/env bash
#----------------------------------------------------
# Slurm job script for standalone CXL.mem flow on SPR nodes.
#
# Runs the repository-supported workflow:
#   1) Build standalone/cxlmem
#   2) Run standalone/cxlmem/run_variants.sh
#   3) Launch scripts/cxl_slot_sweep.py with recommended concurrency
#
# Submit with:
#   sbatch cxlmem_spr.slurm
#----------------------------------------------------

#SBATCH -J CXLMEM_SPR
#SBATCH -o CXLMEM_SPR.o%j
#SBATCH -e CXLMEM_SPR.e%j
#SBATCH -p spr
#SBATCH -N 1
#SBATCH -n 1
#SBATCH -t 24:00:00
#SBATCH --mail-type=END,FAIL

set -euo pipefail

REPO_ROOT="${SLURM_SUBMIT_DIR:-$PWD}"
if [ ! -d "${REPO_ROOT}/standalone/cxlmem" ] || [ ! -f "${REPO_ROOT}/scripts/cxl_slot_sweep.py" ]; then
  echo "ERROR: run sbatch from repo root (expected standalone/cxlmem and scripts/cxl_slot_sweep.py)." >&2
  exit 1
fi

MAX_PARALLEL="${MAX_PARALLEL:-4}"
if [ "${MAX_PARALLEL}" -gt 4 ]; then
  echo "WARN: MAX_PARALLEL=${MAX_PARALLEL} exceeds recommended default of 4 from scripts/cxl_slot_sweep.py"
fi

cd "${REPO_ROOT}/standalone/cxlmem"
make
./run_variants.sh

cd "${REPO_ROOT}"
python3 scripts/cxl_slot_sweep.py --max-parallel "${MAX_PARALLEL}"
