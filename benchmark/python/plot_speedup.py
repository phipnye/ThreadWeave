import argparse
import json
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
import seaborn as sns
from pathlib import Path
from typing import Any, Optional


def setup_plot_style() -> None:
    """Configure clean, publication-ready plot aesthetics."""
    sns.set_theme(style="whitegrid", palette="tab10")
    plt.rcParams.update(
        {
            "font.family": "sans-serif",
            "font.sans-serif": ["DejaVu Sans", "Arial", "Helvetica"],
            "axes.edgecolor": "#cccccc",
            "axes.linewidth": 1.1,
            "grid.alpha": 0.35,
            "grid.linestyle": "--",
            "figure.autolayout": True,
            "figure.dpi": 300,
        }
    )


def parse_label_metadata(label_str: str) -> dict[str, str]:
    """Parse key=value pairs from Google Benchmark's label field (e.g., 'library=TW;workload=Balanced')."""
    metadata: dict[str, str] = {}

    if not label_str:
        return metadata

    for item in label_str.split(";"):
        if "=" in item:
            k, v = item.split("=", 1)
            metadata[k.strip().lower()] = v.strip()

    return metadata


def parse_benchmark_json(json_path: Path) -> pd.DataFrame:
    """Extract and structure Google Benchmark metrics dynamically."""
    if not json_path.exists():
        raise FileNotFoundError(f"Input JSON file not found: {json_path}")

    with open(json_path, "r") as f:
        data: dict[str, Any] = json.load(f)

    benchmarks: list[dict] = data.get("benchmarks", [])

    if not benchmarks:
        raise ValueError("No benchmark results found in JSON file.")

    has_aggregates: bool = any(b.get("run_type") == "aggregate" for b in benchmarks)
    records: list[dict[str, Any]] = []

    for b in benchmarks:
        # Avoid double counting if repetitions were run
        if has_aggregates and b.get("run_type") != "aggregate":
            continue

        if has_aggregates and b.get("aggregate_name") != "mean":
            continue

        name: str = b.get("name", "")
        parts: list[str] = name.split("/")

        # Expect function_name/arg1/arg2/...
        if len(parts) < 3:
            continue

        func_name: str = parts[0]

        try:
            threads: int = int(parts[1])
            tasks: int = int(parts[2])
        except ValueError:
            continue

        # Extract metadata from JSON label if available
        label_meta: dict[str, str] = parse_label_metadata(b.get("label", ""))
        library: str = label_meta.get("library", func_name)
        workload: str = label_meta.get("workload", "Default")

        # Standardize real_time to milliseconds
        time_val: float = b.get("real_time", b.get("cpu_time", 0.0))
        unit: str = b.get("time_unit", "ms").lower()

        if unit == "ns":
            time_ms = time_val / 1e6
        elif unit in ("us", "µs"):
            time_ms = time_val / 1e3
        elif unit == "s":
            time_ms = time_val * 1e3
        else:
            time_ms = time_val

        records.append(
            {
                "Library": library,
                "Workload": workload,
                "Threads": threads,
                "Tasks": tasks,
                "Time_ms": time_ms,
            }
        )

    df: pd.DataFrame = pd.DataFrame(records)

    if df.empty:
        raise ValueError("Failed to parse benchmark records from JSON output.")

    return df


def compute_speedup(
    df: pd.DataFrame, baseline_lib: Optional[str] = None
) -> tuple[pd.DataFrame, str]:
    """Compute relative speedup dynamically against a baseline library."""
    libraries: np.ndarray[str] = df["Library"].unique()

    if len(libraries) < 2:
        df["Speedup"] = 1.0
        return df

    # Default to first discovered non-baseline library if unspecified
    if not baseline_lib or baseline_lib not in libraries:
        baseline_lib = libraries[1] if len(libraries) > 1 else libraries[0]

    pivot: pd.DataFrame = df.pivot(
        index=["Workload", "Threads", "Tasks"], columns="Library", values="Time_ms"
    ).reset_index()

    other_libs: list[str] = [lib for lib in libraries if lib != baseline_lib]

    for lib in other_libs:
        # Speedup > 1.0 indicates 'lib' is faster than 'baseline_lib'
        pivot[f"Speedup_{lib}_vs_{baseline_lib}"] = pivot[baseline_lib] / pivot[lib]

    return pivot, baseline_lib


def plot_execution_times(df: pd.DataFrame, out_dir: Path) -> None:
    """Plot 1: Faceted Execution Time vs Task Count (Log-Log) across all dynamic thread/workload combinations."""
    g: sns.FacetGrid = sns.FacetGrid(
        df,
        col="Threads",
        row="Workload",
        hue="Library",
        height=3.5,
        aspect=1.2,
        sharey=False,
    )
    g.map_dataframe(
        sns.lineplot, x="Tasks", y="Time_ms", marker="o", linewidth=2, markersize=6
    )
    g.set_axis_labels("Task Count", "Execution Time (ms)")
    g.set_titles(col_template="{col_name} Threads", row_template="{row_name}")

    for ax in g.axes.flat:
        ax.set_xscale("log")
        ax.set_yscale("log")
        ax.grid(True, which="both", linestyle=":", alpha=0.5)

    g.add_legend(title="Library", frameon=True)
    g.figure.subplots_adjust(top=0.88)
    g.figure.suptitle(
        "Execution Time vs Task Count (Log-Log Scale)",
        fontsize=14,
        fontweight="bold",
    )
    output_path: Path = out_dir / "01_execution_time_scaling.png"
    g.savefig(output_path)
    plt.close()
    print(f"Saved plot: {output_path}")


def plot_speedup_heatmaps(
    pivot_df: pd.DataFrame, baseline_lib: str, out_dir: Path
) -> None:
    """Plot 2: Dynamic Speedup Heatmaps relative to baseline_lib."""
    workloads: np.ndarray[str] = pivot_df["Workload"].unique()
    speedup_cols: list[str] = [c for c in pivot_df.columns if c.startswith("Speedup_")]

    if not speedup_cols:
        return

    speedup_col: str = speedup_cols[0]
    target_lib: str = speedup_col.split("_")[1]

    fig, axes = plt.subplots(
        1, len(workloads), figsize=(6 * len(workloads), 4.5), squeeze=False
    )

    def fmt_tasks(val: int) -> str:
        if val >= 1_000_000:
            return f"{val/1_000_000:.1f}M".rstrip(".0")
        if val >= 1_000:
            return f"{val/1_000:.1f}k".rstrip(".0")
        return str(val)

    for i, workload in enumerate(workloads):
        sub_df: pd.DataFrame = pivot_df[pivot_df["Workload"] == workload]

        if sub_df.empty:
            continue

        heatmap_data: pd.DataFrame = sub_df.pivot(
            index="Threads", columns="Tasks", values=speedup_col
        )
        heatmap_data.columns = [fmt_tasks(c) for c in heatmap_data.columns]
        ax = axes[0, i]
        sns.heatmap(
            heatmap_data,
            annot=True,
            fmt=".2f",
            cmap="vlag",
            center=1.0,
            cbar_kws={"label": f"Speedup Ratio ({baseline_lib} / {target_lib})"},
            ax=ax,
            linewidths=1,
            linecolor="white",
            annot_kws={"size": 10, "weight": "bold"},
        )
        ax.set_title(f"Workload: {workload}", fontsize=12, fontweight="bold")
        ax.set_xlabel("Task Count", fontsize=10)
        ax.set_ylabel("Thread Count" if i == 0 else "", fontsize=10)

    fig.suptitle(
        f"Speedup of {target_lib} Relative to {baseline_lib} (>1.0 = {target_lib} Faster)",
        fontsize=13,
        fontweight="bold",
    )
    output_path = out_dir / "02_speedup_heatmaps.png"
    plt.savefig(output_path, bbox_inches="tight")
    plt.close()
    print(f"Saved plot: {output_path}")


def plot_thread_scaling(df: pd.DataFrame, out_dir: Path) -> None:
    """Plot 3: Multithreading Efficiency across thread counts for all tested task sizes."""
    thread_ticks: list[int] = sorted(df["Threads"].unique())
    g: sns.FacetGrid = sns.FacetGrid(
        df,
        col="Tasks",
        row="Workload",
        hue="Library",
        height=3.5,
        aspect=1.2,
        sharey=False,
    )
    g.map_dataframe(
        sns.lineplot, x="Threads", y="Time_ms", marker="s", linewidth=2, markersize=6
    )
    g.set_axis_labels("Thread Count", "Execution Time (ms)")
    g.set_titles(col_template="{col_name} Tasks", row_template="{row_name}")

    for ax in g.axes.flat:
        ax.set_xticks(thread_ticks)  # Dynamically set to observed thread counts
        ax.grid(True, linestyle="--", alpha=0.4)

    g.add_legend(title="Library", frameon=True)
    g.figure.subplots_adjust(top=0.88)
    g.figure.suptitle(
        "Thread Scaling Performance Across All Task Sizes",
        fontsize=14,
        fontweight="bold",
    )
    output_path: Path = out_dir / "03_thread_scaling.png"
    g.savefig(output_path)
    plt.close()
    print(f"Saved plot: {output_path}")


def main() -> None:
    parser: argparse.ArgumentParser = argparse.ArgumentParser(
        description="Analyze Google Benchmark JSON outputs with zero hardcoded assumptions."
    )
    parser.add_argument(
        "json_file", type=Path, help="Path to Google Benchmark JSON output."
    )
    parser.add_argument(
        "-o",
        "--out-dir",
        type=Path,
        default=Path("./benchmark_plots"),
        help="Directory to save output plots.",
    )
    parser.add_argument(
        "--baseline",
        type=str,
        default=None,
        help="Library name to treat as baseline for speedup calculations.",
    )
    args = parser.parse_args()
    args.out_dir.mkdir(parents=True, exist_ok=True)
    setup_plot_style()
    print(f"Loading benchmark JSON: {args.json_file}")
    df = parse_benchmark_json(args.json_file)
    pivot_df, baseline_lib = compute_speedup(df, args.baseline)
    print(f"Extracted Libraries: {df['Library'].unique().tolist()}")
    print(f"Extracted Workloads: {df['Workload'].unique().tolist()}")
    print(f"Extracted Thread Counts: {sorted(df['Threads'].unique())}")
    print(f"Extracted Task Counts: {sorted(df['Tasks'].unique())}")
    print("\nGenerating visual diagnostics...")
    plot_execution_times(df, args.out_dir)
    plot_speedup_heatmaps(pivot_df, baseline_lib, args.out_dir)
    plot_thread_scaling(df, args.out_dir)
    print(f"\nAnalysis complete. Plots saved to: {args.out_dir.resolve()}")


if __name__ == "__main__":
    main()
