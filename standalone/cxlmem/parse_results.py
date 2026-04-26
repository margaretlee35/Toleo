#!/usr/bin/env python3
import argparse
import csv
import re
from pathlib import Path


def natural_key(text: str):
    return [int(tok) if tok.isdigit() else tok.lower() for tok in re.split(r"(\d+)", text)]


def parse_kv(path: Path):
    data = {}
    for line in path.read_text().splitlines():
        if "=" not in line:
            continue
        k, v = line.split("=", 1)
        data[k.strip()] = v.strip()
    return data


def decode_case_name(case: str):
    # Human-friendly metadata for plots/tables.
    mapping = {
        "baseline": ("preset", "all extras off", "0"),
        "m2s_plus1": ("preset", "M2S write extra slot", "1"),
        "s2m_plus1": ("preset", "S2M read-rsp extra slot", "1"),
        "both_plus1": ("preset", "both extra slots", "1"),
    }
    if case in mapping:
        return mapping[case]

    m = re.match(r"^max_outstanding_(\d+)$", case)
    if m:
        return ("sweep", "max_outstanding", m.group(1))

    m = re.match(r"^serdes_(\d+)$", case)
    if m:
        return ("sweep", "serdes_time_per_flit", m.group(1))

    m = re.match(r"^readpct_(\d+)$", case)
    if m:
        return ("sweep", "read_percent", m.group(1))

    return ("other", case, "")


def main():
    ap = argparse.ArgumentParser(description="Parse cxl_mem_sim key=value logs into CSV.")
    ap.add_argument("--input-dir", required=True)
    ap.add_argument("--output-csv", required=True)
    ap.add_argument("--emit-simout-dir", default="")
    args = ap.parse_args()

    input_dir = Path(args.input_dir)
    logs = sorted(input_dir.glob("*.log"), key=lambda p: natural_key(p.stem))
    if not logs:
        raise SystemExit(f"No .log files under {input_dir}")

    rows = []
    all_keys = {"case", "case_group", "case_param", "case_value"}
    for log in logs:
        rec = parse_kv(log)
        rec["case"] = log.stem
        grp, prm, val = decode_case_name(log.stem)
        rec["case_group"] = grp
        rec["case_param"] = prm
        rec["case_value"] = val
        rows.append(rec)
        all_keys.update(rec.keys())

    preferred = [
        "case", "case_group", "case_param", "case_value",
        "completed_requests", "avg_read_latency", "avg_write_latency",
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
