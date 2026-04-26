#!/usr/bin/env python3
import argparse
import csv
import re
from pathlib import Path




def natural_key(text: str):
    return [int(tok) if tok.isdigit() else tok.lower() for tok in re.split(r"(\d+)", text)]

def load_rows(csv_path: Path):
    with csv_path.open() as f:
        rows = list(csv.DictReader(f))
    rows.sort(key=lambda r: natural_key(r.get("case", "")))
    return rows


def to_float(v, d=0.0):
    try:
        return float(v)
    except Exception:
        return d


def write_text_fallback(rows, out_dir: Path):
    out = out_dir / "plot_fallback.txt"
    with out.open("w") as f:
        f.write("matplotlib not available; numeric summary fallback\n")
        for r in rows:
            f.write(
                f"{r.get('case','?')}: thr={r.get('throughput_reqs_per_cycle','')}, "
                f"m2s_flits={r.get('total_m2s_flits','')}, s2m_flits={r.get('total_s2m_flits','')}, "
                f"latR={r.get('avg_read_latency','')}, latW={r.get('avg_write_latency','')}\n"
            )


def plot(rows, out_dir: Path):
    import matplotlib.pyplot as plt

    cases = [r.get("case", "") for r in rows]
    thr = [to_float(r.get("throughput_reqs_per_cycle")) for r in rows]
    lat_r = [to_float(r.get("avg_read_latency")) for r in rows]
    lat_w = [to_float(r.get("avg_write_latency")) for r in rows]
    m2s = [to_float(r.get("total_m2s_flits")) for r in rows]
    s2m = [to_float(r.get("total_s2m_flits")) for r in rows]

    fig, ax = plt.subplots(figsize=(12, 4))
    ax.bar(cases, thr)
    ax.set_title("Throughput by case")
    ax.set_ylabel("reqs/cycle")
    ax.tick_params(axis="x", labelrotation=60)
    fig.tight_layout()
    fig.savefig(out_dir / "throughput.png", dpi=150)
    plt.close(fig)

    fig, ax = plt.subplots(figsize=(12, 4))
    ax.plot(cases, lat_r, marker="o", label="avg_read_latency")
    ax.plot(cases, lat_w, marker="o", label="avg_write_latency")
    ax.set_title("Latency by case")
    ax.set_ylabel("cycles")
    ax.legend()
    ax.tick_params(axis="x", labelrotation=60)
    fig.tight_layout()
    fig.savefig(out_dir / "latency.png", dpi=150)
    plt.close(fig)

    fig, ax = plt.subplots(figsize=(12, 4))
    x = range(len(cases))
    ax.bar(x, m2s, label="M2S flits")
    ax.bar(x, s2m, bottom=m2s, label="S2M flits")
    ax.set_xticks(list(x))
    ax.set_xticklabels(cases, rotation=60)
    ax.set_title("Flit volume by case")
    ax.legend()
    fig.tight_layout()
    fig.savefig(out_dir / "flits.png", dpi=150)
    plt.close(fig)


def main():
    ap = argparse.ArgumentParser(description="Generate plots from cxlmem summary.csv")
    ap.add_argument("--csv", required=True)
    ap.add_argument("--out-dir", required=True)
    args = ap.parse_args()

    rows = load_rows(Path(args.csv))
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    try:
        plot(rows, out_dir)
        print(f"Wrote plots to {out_dir}")
    except Exception as e:
        write_text_fallback(rows, out_dir)
        print(f"Plot generation fallback used: {e}")


if __name__ == "__main__":
    main()
