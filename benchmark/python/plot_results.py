"""
plot_results.py

Parses Google Benchmark JSON output from ThreadWeave's benchmarking (threadPoolOverheadBM, sequentialSweepBM,
threadPerTaskSweepBM, stdAsyncBM, poolBM) and produces:

1. throughput_vs_granularity.png - throughput vs. task size, one line per thread count (poolBM only)
2. speedup_vs_threads.png - speedup vs. thread count, one line per task granularity (poolBM vs. sequentialSweepBM), 
with an ideal-linear reference line
3. baseline_comparison.png - poolBM (max threads) vs. sequentialSweepBM vs. stdAsyncBM vs. threadPerTaskSweepBM, bar
chart across granularities
4. overhead_latency.png - threadPoolOverheadBM: time/op vs. thread count

Usage:
    ./BenchProgram \
        --benchmark_repetitions=5 \
        --benchmark_report_aggregates_only=true \
        --benchmark_out=results.json \
        --benchmark_out_format=json

python3 plot_results.py results.json
"""

import json
import os
import sys
import matplotlib.pyplot as plt
from collections import defaultdict
from typing import Any


def OUTPUT(x: str = "") -> str:
    return os.path.join(os.path.dirname(__file__), "output", x)


def load(path: str) -> list[dict[str, Any]]:
    with open(OUTPUT(path)) as f:
        data: dict[str, Any] = json.load(f)

    return data["benchmarks"]


def is_real_run(entry: dict[str, Any]) -> bool:
    return entry.get("run_type") != "aggregate" or entry.get("aggregate_name") == "mean"


def items_per_second(entry: dict[str, Any]) -> float:
    return entry.get("items_per_second", 0.0)


def real_time(entry: dict[str, Any]) -> float:
    return entry.get("real_time", 0.0)


def parse_sweep(
    benchmarks: list[dict[str, Any]], name: str, has_threads: bool
) -> dict[int | tuple[int, int], float]:
    out: dict[int | tuple[int, int], float] = {}

    for e in benchmarks:
        if not e["name"].startswith(name) or not is_real_run(e):
            continue

        n_iter = int(e["nIter"])

        if has_threads:
            n_threads = int(e["nThreads"])
            out[(n_iter, n_threads)] = items_per_second(e)
        else:
            out[n_iter] = items_per_second(e)

    return out


def parse_overhead(
    benchmarks: dict[str, Any], name: str = "threadPoolOverheadBM"
) -> dict[int, float]:
    out: dict[int, float] = {}

    for e in benchmarks:
        if not e["name"].startswith(name) or not is_real_run(e):
            continue

        parts = e["name"].split("/")
        n_threads = int(parts[1]) if len(parts) > 1 and parts[1].isdigit() else None

        if n_threads is None:
            continue

        out[n_threads] = real_time(e)

    return out


def plot_overhead(
    benchmarks: list[dict[str, Any]], outpath: str = OUTPUT("overhead_latency.png")
) -> None:
    data: dict[int, float] = parse_overhead(benchmarks)

    if not data:
        print("No threadPoolOverheadBM results found, skipping overhead plot.")
        return

    xs: list[int] = sorted(data.keys())
    ys: list[float] = [data[x] for x in xs]
    plt.figure(figsize=(7, 5))
    plt.plot(xs, ys, marker="o")
    plt.xlabel("Thread count")
    plt.ylabel("Time per submit + get (microseconds)")
    plt.title("Thread Pool Scheduling Overhead")
    plt.grid(True, alpha=0.3)
    plt.tight_layout()
    plt.savefig(outpath, dpi=150)
    print(f"Wrote {outpath}")


def plot_throughput_vs_granularity(
    pool_data: dict[int | tuple[int, int], float],
    outpath: str = OUTPUT("throughput_vs_granularity.png"),
) -> None:
    by_threads: defaultdict[int, tuple[int, float]] = defaultdict(list)

    for (n_iter, n_threads), ips in pool_data.items():
        by_threads[n_threads].append((n_iter, ips))

    plt.figure(figsize=(8, 6))

    for n_threads, points in sorted(by_threads.items()):
        points.sort()
        xs: list[int] = [p[0] for p in points]
        ys: list[float] = [p[1] for p in points]
        plt.plot(xs, ys, marker="o", label=f"{n_threads} threads")

    plt.xscale("log")
    plt.yscale("log")
    plt.xlabel("Task granularity (busyWork iterations)")
    plt.ylabel("Throughput (tasks/sec)")
    plt.title("ThreadWeave Pool Throughput vs. Task Granularity")
    plt.legend()
    plt.grid(True, which="both", alpha=0.3)
    plt.tight_layout()
    plt.savefig(outpath, dpi=150)
    print(f"Wrote {outpath}")


def plot_speedup_vs_threads(
    pool_data: dict[int | tuple[int, int], float],
    seq_data: dict[int | tuple[int, int], float],
    outpath: str = OUTPUT("speedup_vs_threads.png"),
) -> None:
    by_granularity: defaultdict[int, dict[int, float]] = defaultdict(dict)

    for (n_iter, n_threads), ips in pool_data.items():
        by_granularity[n_iter][n_threads] = ips

    plt.figure(figsize=(8, 6))

    for n_iter, thread_map in sorted(by_granularity.items()):
        baseline: int | float = seq_data.get(n_iter, 0)

        if baseline == 0:
            continue

        xs: list[int] = []
        ys: list[float] = []

        for n_threads, ips in sorted(thread_map.items()):
            xs.append(n_threads)
            ys.append(ips / baseline)

        plt.plot(xs, ys, marker="o", label=f"{n_iter} iters/task")

    if by_granularity:
        max_threads: int = max(t for tm in by_granularity.values() for t in tm.keys())
        plt.plot(
            [1, max_threads], [1, max_threads], "k--", alpha=0.4, label="ideal linear"
        )

    plt.xlabel("Thread count")
    plt.ylabel("Speedup vs. sequential")
    plt.title("ThreadWeave Pool Speedup vs. Thread Count")
    plt.legend()
    plt.grid(True, alpha=0.3)
    plt.tight_layout()
    plt.savefig(outpath, dpi=150)
    print(f"Wrote {outpath}")


def plot_baseline_comparison(
    benchmarks: list[dict[str, Any]], outpath: str = OUTPUT("baseline_comparison.png")
) -> None:
    pool: dict[int | tuple[int, int], float] = parse_sweep(
        benchmarks, "poolBM", has_threads=True
    )
    tpt: dict[int | tuple[int, int], float] = parse_sweep(
        benchmarks, "threadPerTaskSweepBM", has_threads=False
    )
    seq: dict[int | tuple[int, int], float] = parse_sweep(
        benchmarks, "sequentialSweepBM", has_threads=False
    )
    stdasync: dict[int | tuple[int, int], float] = parse_sweep(
        benchmarks, "stdAsyncBM", has_threads=False
    )

    if not pool:
        print("No poolBM results found. Skipping baseline comparison.")
        return

    max_threads: int = max(t for (_, t) in pool.keys())
    granularities: list[int | tuple[int, int]] = sorted(seq.keys())
    pool_series: list[float] = [pool.get((g, max_threads), 0) for g in granularities]
    tpt_series: list[float] = [tpt.get(g, 0) for g in granularities]
    seq_series: list[float] = [seq.get(g, 0) for g in granularities]
    stdasync_series: list[float] = [stdasync.get(g, 0) for g in granularities]
    x: range[int] = range(len(granularities))
    width: float = 0.2
    plt.figure(figsize=(10, 6))
    plt.bar([i - 1.5 * width for i in x], seq_series, width, label="Sequential")
    plt.bar([i - 0.5 * width for i in x], stdasync_series, width, label="std::async")
    plt.bar([i + 0.5 * width for i in x], tpt_series, width, label="thread-per-task")
    plt.bar(
        [i + 1.5 * width for i in x],
        pool_series,
        width,
        label=f"ThreadWeave ({max_threads} threads)",
    )
    plt.xticks(list(x), [str(g) for g in granularities])
    plt.yscale("log")
    plt.xlabel("Task granularity (busyWork iterations)")
    plt.ylabel("Throughput (tasks/sec, log scale)")
    plt.title("Throughput: ThreadWeave Pool vs. Baselines")
    plt.legend()
    plt.tight_layout()
    plt.savefig(outpath, dpi=150)
    print(f"Wrote {outpath}")


def main() -> None:
    benchmarks: list[dict[str, Any]] = load(sys.argv[1])
    pool_data: dict[int | tuple[int, int], float] = parse_sweep(
        benchmarks, "poolBM", has_threads=True
    )

    if not pool_data:
        print(
            "No poolBM results found in JSON. Make you run benchmarking with --benchmark_out."
        )
        sys.exit(1)

    seq_data: dict[int | tuple[int, int], float] = parse_sweep(
        benchmarks, "sequentialSweepBM", has_threads=False
    )
    plot_overhead(benchmarks)
    plot_throughput_vs_granularity(pool_data)
    plot_speedup_vs_threads(pool_data, seq_data)
    plot_baseline_comparison(benchmarks)


if __name__ == "__main__":
    main()
