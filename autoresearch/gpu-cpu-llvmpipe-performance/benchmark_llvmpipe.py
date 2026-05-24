#!/usr/bin/env python3
"""Benchmark st's GPU renderer under Mesa llvmpipe against Xft.

This benchmark is the objective gate for autoresearch/gpu-cpu-llvmpipe-performance.
It measures the CPU-backed GPU renderer (LIBGL_ALWAYS_SOFTWARE=1,
GALLIUM_DRIVER=llvmpipe) against an Xft build of the same source tree.

Acceptance guidance for experiments:
  * optimize the weighted total score first;
  * wall time is primary, CPU time secondary, RSS tertiary;
  * cursor_updates and repaint have the highest workload weights;
  * do not keep an experiment that improves the total by sacrificing either
    cursor_updates or repaint substantially.
"""
from __future__ import annotations

import argparse
import json
import os
import random
import shutil
import statistics
import subprocess
import sys
import tempfile
import textwrap
import time
from pathlib import Path

WORKLOADS = [
    ("cursor_updates", 120_000, 0.35),
    ("repaint", 1_500, 0.30),
    ("scroll_ascii", 80_000, 0.15),
    ("scroll_unicode", 60_000, 0.10),
    ("scroll_emoji", 25_000, 0.10),
]
WALL_WEIGHT = 0.70
CPU_WEIGHT = 0.25
RSS_WEIGHT = 0.05

WORKLOAD_PY = r'''
#!/usr/bin/env python3
import sys, time
kind = sys.argv[1]
n = int(sys.argv[2])
out = sys.stdout.buffer
def w(s): out.write(s.encode('utf-8'))
if kind == 'scroll_ascii':
    for i in range(n):
        w(f'{i:06d} The quick brown fox jumps over the lazy dog 0123456789 abcdefghijklmnopqrstuvwxyz\r\n')
elif kind == 'scroll_unicode':
    for i in range(n):
        w(f'{i:06d} αβγδεζη θικλμν ξοπρστυφχψω 日本語 한글 café naïve résumé — ✓\r\n')
elif kind == 'scroll_emoji':
    line = '😀 😃 😄 😁 😆 😅 😂 🙂 🙃 😉 😊 😇 🐱 🚀 ✨ ❤️ '
    for i in range(n):
        w(f'{i:06d} {line}\r\n')
elif kind == 'repaint':
    row = 'repaint frame {i:06d} αβγδεζη 日本語 café ✓ -- The quick brown fox jumps over the lazy dog     '
    for i in range(n):
        w('\x1b[H')
        for r in range(40):
            w((row.format(i=i)[:110]).ljust(110) + '\r\n')
elif kind == 'cursor_updates':
    for i in range(n):
        row = i % 40 + 1
        col = (i * 17) % 120 + 1
        color = i % 8
        ch = chr(65 + (i % 26))
        w(f'\x1b[{row};{col}H\x1b[3{color}m{ch}\x1b[0m')
else:
    raise SystemExit(f'unknown workload {kind}')
out.flush()
time.sleep(0.05)
'''


def run(cmd, *, cwd: Path, env=None, stdout=None):
    subprocess.run(cmd, cwd=cwd, env=env, check=True, stdout=stdout or subprocess.DEVNULL, stderr=subprocess.STDOUT)


def repo_root() -> Path:
    return Path(subprocess.check_output(["git", "rev-parse", "--show-toplevel"], text=True).strip())


def build_binaries(root: Path, outdir: Path) -> dict[str, Path]:
    config = root / "config.h"
    original = config.read_text()
    bins = {}
    try:
        run(["make"], cwd=root, stdout=(outdir / "build-gpu.log").open("w"))
        bins["gpu_llvmpipe"] = outdir / "st-gpu-llvmpipe"
        shutil.copy2(root / "st", bins["gpu_llvmpipe"])

        if "static int gpudraw = 1;" not in original:
            raise SystemExit("config.h does not contain expected `static int gpudraw = 1;`")
        config.write_text(original.replace("static int gpudraw = 1;", "static int gpudraw = 0;"))
        run(["make"], cwd=root, stdout=(outdir / "build-xft.log").open("w"))
        bins["xft"] = outdir / "st-xft"
        shutil.copy2(root / "st", bins["xft"])
    finally:
        config.write_text(original)
        run(["make"], cwd=root, stdout=(outdir / "build-restore-gpu.log").open("w"))
    return bins


def start_xenv(name: str) -> str:
    subprocess.run(["xenv", "start", name], check=True)
    time.sleep(1)
    return subprocess.check_output(["xenv", "display", "-e", name], text=True).strip()


def stop_xenv(name: str) -> None:
    subprocess.run(["xenv", "stop", name], check=False)


def run_one(binary: Path, display: str, workload_script: Path, kind: str, n: int) -> dict:
    env = os.environ.copy()
    env.update({
        "DISPLAY": display,
        "LIBGL_ALWAYS_SOFTWARE": "1",
        "GALLIUM_DRIVER": "llvmpipe",
        "LP_NUM_THREADS": env.get("LP_NUM_THREADS", "1"),
    })
    cmd = [str(binary), "-T", f"llvmpipe-{kind}", "-e", sys.executable, str(workload_script), kind, str(n)]
    t0 = time.perf_counter()
    proc = subprocess.Popen(cmd, env=env)
    _, status, ru = os.wait4(proc.pid, 0)
    return {
        "wall": time.perf_counter() - t0,
        "cpu": ru.ru_utime + ru.ru_stime,
        "maxrss": ru.ru_maxrss,
        "status": status,
    }


def median(vals, key):
    return statistics.median(v[key] for v in vals)


def summarize(raw: list[dict]) -> list[dict]:
    out = []
    for kind, n, weight in WORKLOADS:
        row = {"workload": kind, "iterations": n, "weight": weight}
        for impl in ("xft", "gpu_llvmpipe"):
            vals = [r for r in raw if r["workload"] == kind and r["impl"] == impl and r["kept"]]
            row[f"{impl}_wall"] = median(vals, "wall")
            row[f"{impl}_cpu"] = median(vals, "cpu")
            row[f"{impl}_rss"] = median(vals, "maxrss")
        row["gpu_wall_speedup"] = row["xft_wall"] / row["gpu_llvmpipe_wall"]
        row["gpu_cpu_ratio"] = row["gpu_llvmpipe_cpu"] / row["xft_cpu"]
        row["gpu_rss_ratio"] = row["gpu_llvmpipe_rss"] / row["xft_rss"]
        row["score"] = weight * (
            WALL_WEIGHT * row["gpu_wall_speedup"] +
            CPU_WEIGHT * (row["xft_cpu"] / row["gpu_llvmpipe_cpu"]) +
            RSS_WEIGHT * (row["xft_rss"] / row["gpu_llvmpipe_rss"])
        )
        out.append(row)
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--iterations", type=int, default=7, help="total randomized iterations; first two are warmups")
    ap.add_argument("--warmups", type=int, default=2)
    ap.add_argument("--name", default=f"llvmpipe-{int(time.time())}")
    ap.add_argument("--out", type=Path, default=None)
    args = ap.parse_args()

    root = repo_root()
    outdir = args.out or root / "autoresearch" / "gpu-cpu-llvmpipe-performance" / "runs" / time.strftime("%Y%m%d-%H%M%S")
    outdir.mkdir(parents=True, exist_ok=True)
    workload = outdir / "workload.py"
    workload.write_text(WORKLOAD_PY)
    workload.chmod(0o755)

    bins = build_binaries(root, outdir)
    raw = []
    display = start_xenv(args.name)
    try:
        for i in range(args.iterations):
            batch = [(kind, n, impl) for kind, n, _ in WORKLOADS for impl in bins]
            random.Random(0xC0FFEE + i).shuffle(batch)
            for kind, n, impl in batch:
                result = run_one(bins[impl], display, workload, kind, n)
                rec = {"iter": i, "kept": i >= args.warmups, "impl": impl, "workload": kind, **result}
                raw.append(rec)
                print(json.dumps(rec), flush=True)
    finally:
        stop_xenv(args.name)

    summary = summarize(raw)
    total = sum(r["score"] for r in summary)
    doc = {
        "benchmark": "gpu-cpu-llvmpipe-performance",
        "description": "CPU-backed GPU renderer via Mesa llvmpipe vs Xft",
        "env": {"LIBGL_ALWAYS_SOFTWARE": "1", "GALLIUM_DRIVER": "llvmpipe", "LP_NUM_THREADS": os.environ.get("LP_NUM_THREADS", "1")},
        "weights": {"wall": WALL_WEIGHT, "cpu": CPU_WEIGHT, "rss": RSS_WEIGHT},
        "workloads": [{"name": k, "iterations": n, "weight": w} for k, n, w in WORKLOADS],
        "total_score": total,
        "summary": summary,
        "raw": raw,
    }
    (outdir / "result.json").write_text(json.dumps(doc, indent=2))
    print("SUMMARY")
    print(json.dumps({"total_score": total, "summary": summary}, indent=2), flush=True)
    print(f"RESULT {outdir / 'result.json'}")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
