#!/usr/bin/env python3
import argparse
import csv
from pathlib import Path


def parse_kv(path: Path):
    data = {}
    for line in path.read_text().splitlines():
      if "=" not in line:
          continue
      k, v = line.split("=", 1)
      data[k.strip()] = v.strip()
    return data


def main():
    ap = argparse.ArgumentParser(description="Parse cxl_mem_sim key=value logs into CSV.")
    ap.add_argument("--input-dir", required=True)
    ap.add_argument("--output-csv", required=True)
    ap.add_argument("--emit-simout-dir", default="")
    args = ap.parse_args()

    input_dir = Path(args.input_dir)
    logs = sorted(input_dir.glob("*.log"))
    if not logs:
        raise SystemExit(f"No .log files under {input_dir}")

    rows = []
    all_keys = set(["case"])
    for log in logs:
        rec = parse_kv(log)
        rec["case"] = log.stem
        rows.append(rec)
        all_keys.update(rec.keys())

    preferred = [
        "case", "completed_requests", "avg_read_latency", "avg_write_latency",
        "total_m2s_flits", "total_s2m_flits", "avg_flit_bytes_used",
        "avg_flit_slots", "avg_wasted_bytes", "m2s_queue_delay", "s2m_queue_delay",
        "requests_with_changed_flit_count", "throughput_reqs_per_cycle",
    ]
    columns = preferred + sorted(k for k in all_keys if k not in preferred)

    output_csv = Path(args.output_csv)
    output_csv.parent.mkdir(parents=True, exist_ok=True)
    with output_csv.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=columns)
        w.writeheader()
        for r in rows:
            w.writerow(r)

    # Optional compatibility export for tools expecting sim.out-like text artifacts.
    if args.emit_simout_dir:
        simout_dir = Path(args.emit_simout_dir)
        simout_dir.mkdir(parents=True, exist_ok=True)
        for r in rows:
            case = r["case"]
            out = simout_dir / f"{case}.sim.out"
            with out.open("w") as f:
                f.write("# cxlmem compatibility summary\n")
                f.write(f"Case | {case}\n")
                for k in columns:
                    if k == "case":
                        continue
                    if k in r and r[k] != "":
                        f.write(f"{k} | {r[k]}\n")

    print(f"Wrote CSV: {output_csv}")


if __name__ == "__main__":
    main()
