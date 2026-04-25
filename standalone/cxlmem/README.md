# Minimal CXL.mem (256B flit) research shim

This directory implements a standalone, minimal CXL.mem simulation layer intended
for research experiments around slot-count perturbations and queue/occupancy
sensitivity.

## Scope and simplifications

Implemented now:
- 256B flit-mode model.
- Dynamic slot generation per transaction.
- Dynamic flit count determined by a packer (not hardcoded per request type).
- M2S and S2M directional transmit queues and serialization/link delay.
- Receive-side reassembly and completion bookkeeping.
- Minimal DRAM backend wrapper with timing callback interface.
- Synthetic CPU request source for fast functional experiments.

Not implemented now:
- Full CXL protocol compliance.
- Linux/mailbox/enumeration/HDM/coherence/IDE/retry-replay.
- SST-specific integration.

## Build

```bash
cd standalone/cxlmem
make
```

## Run one experiment

```bash
./cxl_mem_sim --num-reqs 400 --read-percent 60 \
  --base-m2s-write-slots 4 \
  --base-s2m-read-rsp-slots 4 \
  --extra-m2s-write 0 --extra-s2m-read-rsp 0
```

## Baseline vs +1 slot variants

Use the helper script:

```bash
./run_variants.sh
```

It runs:
1. baseline
2. M2S write +1 slot
3. S2M read response +1 slot
4. both +1

## Test flow (build + smoke cases)

Run the end-to-end test flow script:

```bash
./test_flow.sh
```

This script builds the binary and runs baseline plus the three `+1 slot` variants,
then validates key counters are present and non-zero.

## Key runtime parameters

Supported parameters include:
- `--base-m2s-write-slots`
- `--base-s2m-read-rsp-slots`
- `--extra-m2s-write`
- `--extra-s2m-read-rsp`
- `--packetize-delay`
- `--depacketize-delay`
- `--serdes-time-per-flit`
- `--link-latency`
- `--backend-read-latency`
- `--backend-write-latency`
- `--max-outstanding`
- `--flush-policy` (`0=on_demand`, `1=always`)

## Output stats

The binary reports:
- total M2S/S2M flits
- slot totals by message type
- extra-slot count
- average flit occupancy and wasted bytes
- queue delay per direction
- average read/write latency
- request count with changed flit count (vs baseline slot recipe)
- throughput (`reqs/cycle` in this simple event-time model)
