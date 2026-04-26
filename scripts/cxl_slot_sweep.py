#!/usr/bin/env python3
"""Run a full CXL slot/latency sweep pipeline.

Pipeline:
1) Build/run standalone/cxlmem for read_percent in {25,50,75}.
2) Parse standalone latency + flit efficiency stats.
3) Use read_percent=50 as baseline and compute read/write deltas.
4) Discover CXL latency key(s) in each architecture cfg and emit patched cfg copies under
   results/patched_configs/rp<read_percent>/<arch>.cfg (never touching config/ originals).
5) Launch Sniper jobs through ../run_toleo_sim.py in parallel, one per (read_percent,arch,bench).
6) Parse sim.out for Time(ns), average cxl access latency, average cxl queueing delay, and IPC.
7) Emit results/sweep_results.csv and grouped summary text + stdout table.
"""

import argparse
import csv
import os
import re
import subprocess
import sys
import time
from concurrent.futures import FIRST_COMPLETED, ThreadPoolExecutor, wait
from pathlib import Path
from typing import Dict, List, Optional, Tuple

READ_PERCENTS = [25, 50, 75]
CSV_COLUMNS = [
    "read_percent", "arch", "bench", "sim_time_ns", "avg_cxl_latency_ns",
    "avg_cxl_queue_delay_ns", "avg_ipc", "standalone_read_latency_ns",
    "standalone_write_latency_ns", "delta_read_ns", "delta_write_ns",
    "avg_flit_utilization_pct", "avg_bytes_wasted_per_flit", "status",
]


def run_cmd(cmd: List[str], cwd: Path, dry_run: bool = False) -> subprocess.CompletedProcess:
    print("$", " ".join(cmd))
    if dry_run:
        return subprocess.CompletedProcess(cmd, 0, "", "")
    return subprocess.run(cmd, cwd=str(cwd), text=True, capture_output=True)


def parse_kv_stdout(text: str) -> Dict[str, float]:
    out: Dict[str, float] = {}
    for line in text.splitlines():
        if ":" not in line:
            continue
        k, v = line.split(":", 1)
        k = k.strip()
        try:
            out[k] = float(v.strip())
        except ValueError:
            continue
    return out


def standalone_sweep(repo_root: Path, dry_run: bool) -> Dict[int, Dict[str, float]]:
    sim_dir = repo_root / "standalone" / "cxlmem"
    bin_path = sim_dir / "cxl_mem_sim"
    if not bin_path.exists():
        res = run_cmd(["make"], sim_dir, dry_run=dry_run)
        if res.returncode != 0:
            raise RuntimeError(f"Standalone build failed\nSTDOUT:\n{res.stdout}\nSTDERR:\n{res.stderr}")

    data: Dict[int, Dict[str, float]] = {}
    for rp in READ_PERCENTS:
        cmd = [str(bin_path), "--read-percent", str(rp)]
        res = run_cmd(cmd, sim_dir, dry_run=dry_run)
        if res.returncode != 0:
            raise RuntimeError(f"Standalone run failed for rp={rp}\nSTDOUT:\n{res.stdout}\nSTDERR:\n{res.stderr}")
        parsed = parse_kv_stdout(res.stdout)
        if dry_run:
            parsed = {
                "avg_read_latency_ns": 0.0,
                "avg_write_latency_ns": 0.0,
                "avg_flit_utilization_pct": 0.0,
                "avg_bytes_wasted_per_flit": 0.0,
            }
        for req_key in ["avg_read_latency_ns", "avg_write_latency_ns", "avg_flit_utilization_pct", "avg_bytes_wasted_per_flit"]:
            if req_key not in parsed:
                raise RuntimeError(f"Standalone output missing {req_key} for rp={rp}")
        data[rp] = parsed
    return data


def discover_latency_keys(cfg_text: str) -> List[str]:
    current = ""
    keys = []
    for raw in cfg_text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        m = re.match(r"\[(.+?)\]", line)
        if m:
            current = m.group(1)
            continue
        if "=" not in line:
            continue
        key, _ = [x.strip() for x in line.split("=", 1)]
        if key != "latency":
            continue
        if current.startswith("perf_model/cxl/memory_expander_") and "/dram" not in current:
            keys.append(f"{current}/latency")
        if current == "perf_model/cxl/vnserver":
            keys.append(f"{current}/latency")
    return keys


def patch_cfg(orig_cfg: Path, out_cfg: Path, delta_read: float, delta_write: float) -> Tuple[List[str], Optional[str]]:
    text = orig_cfg.read_text()
    keys = discover_latency_keys(text)
    if not keys:
        raise RuntimeError(f"Could not find CXL latency key in {orig_cfg}")

    warning = None
    lines = text.splitlines()
    current = ""
    hit = 0
    for i, raw in enumerate(lines):
        line = raw.strip()
        m = re.match(r"\[(.+?)\]", line)
        if m:
            current = m.group(1)
            continue
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, v = [x.strip() for x in line.split("=", 1)]
        path = f"{current}/{k}"
        if k == "latency" and path in keys:
            num = float(v.split("#", 1)[0].strip())
            new_num = num + delta_read
            suffix = ""
            if "#" in raw:
                suffix = " #" + raw.split("#", 1)[1]
            lines[i] = re.sub(r"=.+", f"= {new_num:.6f}{suffix}", raw)
            hit += 1

    if hit == 1 and abs(delta_write - delta_read) > 1e-9:
        warning = "shared latency key patched with read delta; write delta approximated"

    out_cfg.parent.mkdir(parents=True, exist_ok=True)
    out_cfg.write_text("\n".join(lines) + "\n")
    return keys, warning


def parse_metric_line(line: str) -> List[float]:
    nums = re.findall(r"[-+]?\d*\.?\d+(?:[eE][-+]?\d+)?", line)
    return [float(x) for x in nums]


def parse_sim_out(sim_out: Path) -> Optional[Dict[str, float]]:
    if not sim_out.exists():
        return None
    content = sim_out.read_text(errors="ignore").splitlines()
    time_vals = []
    ipc_vals = []
    cxl_lat = None
    cxl_q = None
    for line in content:
        ll = line.lower()
        if "time (ns)" in ll:
            vals = parse_metric_line(line)
            if vals:
                time_vals = vals
        elif re.search(r"\bipc\b", ll):
            vals = parse_metric_line(line)
            if vals:
                ipc_vals = vals
        elif "average cxl access latency (ns)" in ll:
            vals = parse_metric_line(line)
            if vals:
                cxl_lat = vals[0]
        elif "average cxl queueing delay" in ll:
            vals = parse_metric_line(line)
            if vals:
                cxl_q = vals[0]

    if not time_vals or not ipc_vals or cxl_lat is None or cxl_q is None:
        return None
    return {
        "sim_time_ns": max(time_vals),
        "avg_ipc": sum(ipc_vals) / len(ipc_vals),
        "avg_cxl_latency_ns": cxl_lat,
        "avg_cxl_queue_delay_ns": cxl_q,
    }


def has_time_line(sim_out: Path) -> bool:
    if not sim_out.exists():
        return False
    for line in sim_out.read_text(errors="ignore").splitlines():
        if "Time (ns)" in line:
            return True
    return False


def find_existing_simout(bench_dir: Path, rp: int, arch: str) -> Optional[Path]:
    patt = f"sim-sweep-rp{rp}-{arch}"
    for p in bench_dir.glob(f"{patt}/**/sim.out"):
        if has_time_line(p):
            return p
    return None


def build_toleo_commands(run_script: Path, bench: str, arch: str, icount: int) -> List[List[str]]:
    return [
        [
            str(run_script), "sniper", "--bench", bench, "--arch", arch, "-a",
            "--", "-s", f"stop-by-icount:{icount}", "--no-cache-warming",
        ],
        [
            str(run_script), "sniper", "--bench", bench, "--arch", arch, "-a",
            "--sniper-args", f"-s stop-by-icount:{icount} --no-cache-warming",
        ],
    ]


def run_job(
    repo_root: Path,
    run_script: Path,
    bench: str,
    arch: str,
    rp: int,
    cfg_path: Path,
    icount: int,
    dry_run: bool,
) -> Tuple[str, Optional[Path], str]:
    bench_root = repo_root / bench
    out_dir = bench_root / f"sim-sweep-rp{rp}-{arch}"
    out_dir.mkdir(parents=True, exist_ok=True)

    existing = find_existing_simout(bench_root, rp, arch)
    if existing is not None:
        print(f"[SKIP] rp={rp} arch={arch} bench={bench}: found {existing}")
        return ("SKIP", existing, "")

    env = os.environ.copy()
    env["TOLEO_CFG_OVERRIDE"] = str(cfg_path)
    cmds = build_toleo_commands(run_script, bench, arch, icount)
    print("$", " ".join(cmds[0]), f"# cfg={cfg_path}")
    if dry_run:
        return ("DRY_RUN", None, "")

    cp = None
    for idx, cmd in enumerate(cmds):
        cp = subprocess.run(cmd, cwd=str(repo_root), text=True, capture_output=True, env=env)
        if cp.returncode == 0:
            break
        if idx == 0:
            print(f"[WARN] first run_toleo_sim invocation failed for {bench}/{arch}; retrying alternate passthrough mode")
    if cp is None or cp.returncode != 0:
        return ("FAILED", None, f"STDOUT:\n{cp.stdout if cp else ''}\nSTDERR:\n{cp.stderr if cp else ''}")

    # Prefer deterministic sweep output dir if runner honors it; otherwise search latest sim.out.
    cand = list(out_dir.glob("**/sim.out"))
    if cand:
        return ("OK", sorted(cand, key=lambda p: p.stat().st_mtime)[-1], "")

    all_sims = sorted(bench_root.glob(f"sim-*-{arch}/**/sim.out"), key=lambda p: p.stat().st_mtime)
    return ("OK", all_sims[-1] if all_sims else None, "")


def format_summary(rows: List[Dict[str, object]]) -> str:
    grouped: Dict[Tuple[int, str], List[Dict[str, object]]] = {}
    for r in rows:
        grouped.setdefault((int(r["read_percent"]), str(r["arch"])), []).append(r)

    table_rows = []
    for (rp, arch), sub in sorted(grouped.items()):
        ok = [x for x in sub if x["status"] == "OK"]
        sim_time = sum(float(x["sim_time_ns"] or 0) for x in ok) / max(1, len(ok))
        ipc = sum(float(x["avg_ipc"] or 0) for x in ok) / max(1, len(ok))
        cxl = sum(float(x["avg_cxl_latency_ns"] or 0) for x in ok) / max(1, len(ok))
        table_rows.append([rp, arch, len(ok), len(sub), f"{sim_time:.3f}", f"{cxl:.3f}", f"{ipc:.4f}"])

    headers = ["read_percent", "arch", "ok", "total", "mean_time_ns", "mean_cxl_lat_ns", "mean_ipc"]
    try:
        from tabulate import tabulate  # type: ignore
        return tabulate(table_rows, headers=headers, tablefmt="github")
    except Exception:
        widths = [max(len(str(x)) for x in [h] + [r[i] for r in table_rows]) for i, h in enumerate(headers)]
        lines = ["  ".join(h.ljust(widths[i]) for i, h in enumerate(headers))]
        lines.append("  ".join("-" * w for w in widths))
        for r in table_rows:
            lines.append("  ".join(str(r[i]).ljust(widths[i]) for i in range(len(headers))))
        return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bench", nargs="+", default=["bsw-s", "pr-kron-s"])
    ap.add_argument("--arch", nargs="+", default=["zen4_cxl", "zen4_vn"])
    ap.add_argument("--max-parallel", type=int, default=4, help="Max concurrent Toleo jobs (default: 4).")
    ap.add_argument("--icount", type=int, default=100000000, help="Sniper stop-by-icount value (default: 100000000).")
    ap.add_argument(
        "--dry-run",
        action="store_true",
        help="Print commands without running standalone or Toleo simulations (default: disabled).",
    )
    args = ap.parse_args()
    if args.max_parallel > 4:
        print("[WARN] --max-parallel exceeds the recommended default of 4.")
    cpu_count = os.cpu_count() or 1
    slurm_cpus_per_task = os.environ.get("SLURM_CPUS_PER_TASK")
    effective_cpu_budget = int(slurm_cpus_per_task) if slurm_cpus_per_task and slurm_cpus_per_task.isdigit() else cpu_count
    if args.max_parallel > effective_cpu_budget:
        print(
            f"[WARN] --max-parallel={args.max_parallel} exceeds available CPU budget={effective_cpu_budget}; "
            "consider lowering parallelism."
        )

    t0 = time.time()
    repo_root = Path.cwd().resolve()
    run_script = (repo_root.parent / "run_toleo_sim.py").resolve()
    if not run_script.exists() and not args.dry_run:
        print(f"ERROR: expected {run_script}", file=sys.stderr)
        return 2

    results_dir = repo_root / "results"
    results_dir.mkdir(exist_ok=True)

    standalone = standalone_sweep(repo_root, args.dry_run)
    base = standalone[50]

    cfg_map = {a: repo_root / "config" / f"{a}.cfg" for a in args.arch}
    patched_cfgs: Dict[Tuple[int, str], Path] = {}
    for rp in READ_PERCENTS:
        for arch in args.arch:
            src = cfg_map[arch]
            dst = results_dir / "patched_configs" / f"rp{rp}" / f"{arch}.cfg"
            if rp == 50:
                dst.parent.mkdir(parents=True, exist_ok=True)
                if not args.dry_run:
                    dst.write_text(src.read_text())
                patched_cfgs[(rp, arch)] = dst
                continue
            d_read = standalone[rp]["avg_read_latency_ns"] - base["avg_read_latency_ns"]
            d_write = standalone[rp]["avg_write_latency_ns"] - base["avg_write_latency_ns"]
            if not args.dry_run:
                _, warn = patch_cfg(src, dst, d_read, d_write)
                if warn:
                    print(f"[WARN] {arch} rp={rp}: {warn}")
            patched_cfgs[(rp, arch)] = dst

    rows: List[Dict[str, object]] = []
    jobs = []
    for rp in READ_PERCENTS:
        for arch in args.arch:
            for bench in args.bench:
                jobs.append((rp, arch, bench, patched_cfgs[(rp, arch)]))

    with ThreadPoolExecutor(max_workers=args.max_parallel) as ex:
        futmap = {
            ex.submit(run_job, repo_root, run_script, bench, arch, rp, cfg, args.icount, args.dry_run): (rp, arch, bench)
            for rp, arch, bench, cfg in jobs
        }
        completed_jobs = 0
        total_jobs = len(jobs)
        while futmap:
            done, _ = wait(list(futmap.keys()), return_when=FIRST_COMPLETED)
            for fut in done:
                rp, arch, bench = futmap.pop(fut)
                status, sim_out, err = fut.result()
                completed_jobs += 1

                row = {
                    "read_percent": rp,
                    "arch": arch,
                    "bench": bench,
                    "sim_time_ns": "",
                    "avg_cxl_latency_ns": "",
                    "avg_cxl_queue_delay_ns": "",
                    "avg_ipc": "",
                    "standalone_read_latency_ns": standalone[rp]["avg_read_latency_ns"],
                    "standalone_write_latency_ns": standalone[rp]["avg_write_latency_ns"],
                    "delta_read_ns": standalone[rp]["avg_read_latency_ns"] - base["avg_read_latency_ns"],
                    "delta_write_ns": standalone[rp]["avg_write_latency_ns"] - base["avg_write_latency_ns"],
                    "avg_flit_utilization_pct": standalone[rp]["avg_flit_utilization_pct"],
                    "avg_bytes_wasted_per_flit": standalone[rp]["avg_bytes_wasted_per_flit"],
                    "status": status,
                }

                if status == "OK" and sim_out is not None:
                    parsed = parse_sim_out(sim_out)
                    if parsed is None:
                        row["status"] = "PARSE_ERROR"
                    else:
                        row.update(parsed)
                elif status == "FAILED":
                    print(f"[FAIL] rp={rp} arch={arch} bench={bench}\n{err}")

                rows.append(row)
                elapsed = time.time() - t0
                avg_job = elapsed / completed_jobs
                eta = avg_job * (total_jobs - completed_jobs)
                print(f"[PROGRESS] {completed_jobs}/{total_jobs} done | elapsed={elapsed:.1f}s | eta={eta:.1f}s")

    rows.sort(key=lambda r: (int(r["read_percent"]), str(r["arch"]), str(r["bench"])))

    csv_path = results_dir / "sweep_results.csv"
    with csv_path.open("w", newline="") as f:
      w = csv.DictWriter(f, fieldnames=CSV_COLUMNS)
      w.writeheader()
      for r in rows:
          w.writerow(r)

    summary = format_summary(rows)
    print(summary)
    summary_path = results_dir / "sweep_summary.txt"
    summary_path.write_text(summary + "\n")

    print(f"[INFO] CSV results: {csv_path}")
    print(f"[INFO] Summary: {summary_path}")
    print(f"[INFO] Per-run simulator outputs: <repo>/<bench>/sim-sweep-rp<read_percent>-<arch>/**/sim.out")
    print(f"Total wall-clock time: {time.time() - t0:.2f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
