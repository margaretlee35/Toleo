# Standalone CXL.mem 68B flit simulator

This standalone model implements a CXL 2.0 style 68-byte flit link for CXL.mem-centric traffic:

- `H5`: M2S read request header flit (packs additional M2S Req headers).
- `H4 + G0`: M2S write request header followed by paired data flit.
- `G4 + G0`: S2M read-response header flit (optionally packs NDR acks) followed by paired data flit.
- `G5`: S2M NDR-only ack flit (max 2 NDR/flit per CXL 1.1+ errata).
- `G6`: S2M DRS-only header flit when no NDR is available to fill G4 generic slots, with up to 3 DRS headers/flit and one paired G0 per DRS.

The model tracks per-direction serialization queueing delay and reports aggregate latency, throughput, and flit efficiency statistics.

## Build

```bash
cd standalone/cxlmem
make
```

## Run

```bash
./cxl_mem_sim --read-percent 50 --num-reqs 400
```

Supported CLI options:

- `--read-percent N`
- `--num-reqs N`
- `--link-latency N`
- `--serdes-time-per-flit N`
- `--backend-read-latency N`
- `--backend-write-latency N`
- `--max-outstanding N`

## Batch sweep helper

```bash
./run_variants.sh
```

It emits:

```text
read_percent=25
...
read_percent=50
...
read_percent=75
...
```

so downstream scripts can parse each variant cleanly.
