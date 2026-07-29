"""
Analyze Google Benchmark JSON output for the ThreadWeave benchmark suite.

Routes results to the correct analysis based on the `category` field
encoded in each benchmark's label (e.g. "category=Speedup;library=..."):

  - Comparison: ThreadWeave vs every other library's execution time
  - Speedup:    single-library T_1 / T_p parallel speedup + efficiency
  - Latency:    single-task roundtrip latency & amortized task service time
"""

import argparse
import json
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
import seaborn as sns
from pathlib import Path
from typing import Any

# The library this whole suite exists to showcase. Every Comparison-category
# plot is anchored on this name; >1.0 always means ThreadWeave is faster.
THREADWEAVE_LIB: str = "ThreadWeave"


def setup_plot_style() -> None:
    """Configure clean, publication-ready plot aesthetics."""
    sns.set_theme(style="whitegrid", palette="Set1")
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


def fmt_tasks(val: int | float) -> str:
    """Format large task counts nicely (e.g. 10000 -> '10k')."""
    val = int(val)
    if val >= 1_000_000:
        return f"{val / 1_000_000:.1f}M".rstrip("0").rstrip(".")
    if val >= 1_000:
        return f"{val / 1_000:.1f}k".rstrip("0").rstrip(".")
    return str(val)


def parse_label_metadata(label_str: str) -> dict[str, str]:
    """Parse key=value pairs from Google Benchmark's label field
    (e.g., 'category=Speedup;library=ThreadWeave;workload=Balanced')."""
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

        # Allow func_name/threads (len == 2) or func_name/threads/tasks (len == 3)
        if len(parts) < 2:
            continue

        func_name: str = parts[0]

        try:
            threads: int = int(parts[1])
            tasks: int = int(parts[2]) if len(parts) > 2 else 1
        except ValueError:
            continue

        # Extract metadata from JSON label string
        label_meta: dict[str, str] = parse_label_metadata(b.get("label", ""))
        category: str = label_meta.get("category", "Unknown")
        library: str = label_meta.get("library", func_name)
        workload: str = label_meta.get("workload", "Default")

        # Standardize real_time
        time_val: float = b.get("real_time")
        unit: str = b.get("time_unit", "ms").lower()

        # mean_latency is a Counter (kIsRate | kInvert), always reported in raw seconds
        mean_latency_s: float | None = b.get("mean_latency")
        latency_ms: float = (
            mean_latency_s * 1e3 if mean_latency_s is not None else np.nan
        )

        if unit == "ns":
            time_ms = time_val / 1e6
        elif unit in ("us", "µs"):
            time_ms = time_val / 1e3
        elif unit == "s":
            time_ms = time_val * 1e3
        else:
            time_ms = time_val

        time_us: float = time_ms * 1000.0
        amortized_us: float = (
            (mean_latency_s * 1e6) if mean_latency_s is not None else np.nan
        )

        records.append(
            {
                "Category": category,
                "Library": library,
                "Workload": workload,
                "Threads": threads,
                "Tasks": tasks,
                "Time_ms": time_ms,
                "Time_us": time_us,
                "Latency_ms": latency_ms,
                "Amortized_us": amortized_us,
            }
        )

    df: pd.DataFrame = pd.DataFrame(records)

    if df.empty:
        raise ValueError("Failed to parse benchmark records from JSON output.")

    if (df["Category"] == "Unknown").any():
        print(
            "Warning: some benchmarks have no 'category=' field in their label; "
            'they will be skipped. Add SetLabel("category=...;...") in the .cpp source.'
        )

    return df


# =========================================================================================
# Comparison category: ThreadWeave vs every other library, at fixed thread counts
# =========================================================================================


def compute_library_ratios(df: pd.DataFrame) -> tuple[pd.DataFrame, list[str]]:
    """Compute every other library's execution time relative to ThreadWeave's."""
    libraries: np.ndarray = df["Library"].unique()

    if THREADWEAVE_LIB not in libraries:
        raise ValueError(
            f"'{THREADWEAVE_LIB}' not found in Comparison data (found: {list(libraries)}). "
            f"Check the library= field in your benchmark labels."
        )

    pivot: pd.DataFrame = df.pivot(
        index=["Workload", "Threads", "Tasks"], columns="Library", values="Time_ms"
    ).reset_index()

    other_libs: list[str] = [lib for lib in libraries if lib != THREADWEAVE_LIB]

    for lib in other_libs:
        # Ratio > 1.0 indicates ThreadWeave is faster than 'lib'
        pivot[f"Speedup_{lib}"] = pivot[lib] / pivot[THREADWEAVE_LIB]

    return pivot, other_libs


def plot_comparison_execution_times(df: pd.DataFrame, out_dir: Path) -> None:
    """Comparison Plot 1: Faceted execution time vs task count (log-log)."""
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
        "Comparison: Execution Time vs Task Count (Log-Log Scale)",
        fontsize=14,
        fontweight="bold",
    )
    output_path: Path = out_dir / "comparison_01_execution_time_scaling.png"
    g.savefig(output_path)
    plt.close()
    print(f"Saved plot: {output_path}")


def plot_comparison_speedup_heatmaps(
        pivot_df: pd.DataFrame, other_libs: list[str], out_dir: Path
) -> None:
    """Comparison Plot 2: Relative-performance heatmaps."""
    workloads: np.ndarray = pivot_df["Workload"].unique()

    fig, axes = plt.subplots(
        len(other_libs),
        len(workloads),
        figsize=(6 * len(workloads), 4.5 * len(other_libs)),
        squeeze=False,
    )

    for row, lib in enumerate(other_libs):
        speedup_col: str = f"Speedup_{lib}"

        for col, workload in enumerate(workloads):
            sub_df: pd.DataFrame = pivot_df[pivot_df["Workload"] == workload]
            ax = axes[row, col]

            if sub_df.empty:
                continue

            heatmap_data: pd.DataFrame = sub_df.pivot(
                index="Threads", columns="Tasks", values=speedup_col
            )
            heatmap_data.columns = [fmt_tasks(c) for c in heatmap_data.columns]
            sns.heatmap(
                heatmap_data,
                annot=True,
                fmt=".2f",
                cmap="vlag",
                center=1.0,
                cbar_kws={"label": f"{THREADWEAVE_LIB} / {lib}"},
                ax=ax,
                linewidths=1,
                linecolor="white",
                annot_kws={"size": 10, "weight": "bold"},
            )
            ax.set_title(
                f"{lib} \u2014 Workload: {workload}", fontsize=11, fontweight="bold"
            )
            ax.set_xlabel("Task Count", fontsize=10)
            ax.set_ylabel("Thread Count" if col == 0 else "", fontsize=10)

    fig.suptitle(
        f"Comparison: {THREADWEAVE_LIB} Relative Performance (>1.0 = {THREADWEAVE_LIB} Faster)",
        fontsize=14,
        fontweight="bold",
    )
    output_path: Path = out_dir / "comparison_02_relative_performance_heatmaps.png"
    plt.savefig(output_path, bbox_inches="tight")
    plt.close()
    print(f"Saved plot: {output_path}")


def plot_comparison_thread_scaling(df: pd.DataFrame, out_dir: Path) -> None:
    """Comparison Plot 3: Execution time vs thread count, faceted by task size."""
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
        ax.set_xticks(thread_ticks)
        ax.grid(True, linestyle="--", alpha=0.4)

    g.add_legend(title="Library", frameon=True)
    g.figure.subplots_adjust(top=0.88)
    g.figure.suptitle(
        "Comparison: Thread Scaling Across All Task Sizes",
        fontsize=14,
        fontweight="bold",
    )
    output_path: Path = out_dir / "comparison_03_thread_scaling.png"
    g.savefig(output_path)
    plt.close()
    print(f"Saved plot: {output_path}")


def plot_comparison_by_task_size(df: pd.DataFrame, out_dir: Path) -> None:
    """Comparison Plot 4: Library x Workload bars grouped by task size."""
    df_plot: pd.DataFrame = df.copy()
    df_plot["Lib_Workload"] = df_plot["Library"] + " (" + df_plot["Workload"] + ")"
    df_plot["Tasks_Formatted"] = df_plot["Tasks"].apply(fmt_tasks)

    sorted_tasks: list[int] = sorted(df_plot["Tasks"].unique())
    col_order: list[str] = [fmt_tasks(t) for t in sorted_tasks]
    thread_order: list[int] = sorted(df_plot["Threads"].unique())

    g = sns.catplot(
        data=df_plot,
        x="Threads",
        y="Time_ms",
        hue="Lib_Workload",
        col="Tasks_Formatted",
        col_order=col_order,
        order=thread_order,
        kind="bar",
        height=4,
        aspect=1.2,
        sharey=False,
    )
    g.set_axis_labels("Thread Count", "Execution Time (ms)")
    g.set_titles(col_template="{col_name} Tasks")

    for ax in g.axes.flat:
        ax.set_ylim(bottom=0)
        ax.grid(True, which="major", axis="y", linestyle=":", alpha=0.6)

    g.add_legend(title="Library & Workload", frameon=True)
    g.figure.subplots_adjust(top=0.85)
    g.figure.suptitle(
        "Comparison: Library vs Workload by Task Size", fontsize=14, fontweight="bold"
    )

    output_path = out_dir / "comparison_04_performance_by_task_size.png"
    g.savefig(output_path)
    plt.close()
    print(f"Saved plot: {output_path}")


def plot_comparison_performance_ratio(
        df: pd.DataFrame, other_libs: list[str], out_dir: Path
) -> None:
    """Comparison Plot 5: Performance Comparison Ratio bar chart."""
    pivot: pd.DataFrame = df.pivot(
        index=["Workload", "Threads", "Tasks"], columns="Library", values="Time_ms"
    ).reset_index()

    for lib in other_libs:
        if lib not in pivot.columns or THREADWEAVE_LIB not in pivot.columns:
            continue

        plot_df: pd.DataFrame = pivot.copy()
        plot_df["speedup"] = plot_df[lib] / plot_df[THREADWEAVE_LIB]
        plot_df["Tasks_Factor"] = plot_df["Tasks"].apply(fmt_tasks)
        task_order: list[str] = [
            fmt_tasks(t) for t in sorted(plot_df["Tasks"].unique())
        ]
        plot_df["Threads"] = plot_df["Threads"].astype(str)

        g: sns.FacetGrid = sns.catplot(
            data=plot_df,
            x="Tasks_Factor",
            order=task_order,
            y="speedup",
            hue="Threads",
            col="Workload",
            kind="bar",
            width=0.7,
            height=4.5,
            aspect=1.1,
            legend_out=True,
        )

        for ax in g.axes.flat:
            ax.axhline(1.0, color="red", linestyle="--", linewidth=1.2)
            ax.grid(True, which="major", axis="y", linestyle="--", alpha=0.35)

        g.set_axis_labels("Number of Tasks", "Comparison Factor (Multiplier)")
        g.set_titles(col_template="Workload: {col_name}")

        if g._legend:
            g._legend.set_title("Threads")

        g.figure.subplots_adjust(top=0.80)
        g.figure.suptitle(
            f"Performance Comparison Ratio ({THREADWEAVE_LIB} Time / {lib} Time)\n"
            f"Bars below the red dashed line indicate {lib} is faster; above means {THREADWEAVE_LIB} is faster",
            fontsize=12,
            fontweight="bold",
        )

        safe_lib_name: str = lib.replace(" ", "_").replace("::", "_")
        output_path: Path = (
                out_dir / f"comparison_05_performance_ratio_{safe_lib_name}.png"
        )
        g.savefig(output_path)
        plt.close()
        print(f"Saved plot: {output_path}")


# =========================================================================================
# Speedup category: single-library parallel speedup (T_1 / T_p) and efficiency
# =========================================================================================


def compute_parallel_speedup(df: pd.DataFrame) -> pd.DataFrame:
    """Compute T_1 / T_p speedup and efficiency."""
    min_threads: int = df["Threads"].min()
    if min_threads != 1:
        print(
            f"Warning: no nThreads=1 baseline found (minimum is {min_threads}); "
            "speedup values will be relative to that instead of a true T_1 baseline."
        )

    baseline: pd.DataFrame = df[df["Threads"] == min_threads][
        ["Library", "Workload", "Tasks", "Time_ms"]
    ].rename(columns={"Time_ms": "T1"})
    merged: pd.DataFrame = df.merge(baseline, on=["Library", "Workload", "Tasks"])
    merged["Speedup"] = merged["T1"] / merged["Time_ms"]
    merged["Efficiency"] = merged["Speedup"] / merged["Threads"]
    return merged


def plot_speedup_curves(df: pd.DataFrame, out_dir: Path) -> None:
    """Speedup Plot 1: Speedup vs thread count against the ideal linear line."""
    thread_ticks: list[int] = sorted(df["Threads"].unique())
    g: sns.FacetGrid = sns.FacetGrid(
        df,
        col="Tasks",
        row="Workload",
        hue="Library",
        height=3.5,
        aspect=1.1,
        sharey=True,
    )
    g.map_dataframe(
        sns.lineplot, x="Threads", y="Speedup", marker="o", linewidth=2, markersize=6
    )

    for ax in g.axes.flat:
        ax.plot(
            thread_ticks,
            thread_ticks,
            linestyle="--",
            color="gray",
            linewidth=1.3,
            label="Ideal (linear)",
        )
        ax.set_xticks(thread_ticks)
        ax.grid(True, linestyle="--", alpha=0.4)

    g.set_axis_labels("Threads (p)", "Speedup (T\u2081 / T\u209a)")
    g.set_titles(col_template="{col_name} Tasks", row_template="{row_name}")
    g.add_legend(title="Library", frameon=True)
    g.figure.subplots_adjust(top=0.88)
    g.figure.suptitle(
        "Speedup: Parallel Speedup vs Thread Count", fontsize=14, fontweight="bold"
    )
    output_path = out_dir / "speedup_01_speedup_curves.png"
    g.savefig(output_path)
    plt.close()
    print(f"Saved plot: {output_path}")


def plot_efficiency_curves(df: pd.DataFrame, out_dir: Path) -> None:
    """Speedup Plot 2: Parallel efficiency (speedup / p) vs thread count."""
    thread_ticks: list[int] = sorted(df["Threads"].unique())
    g: sns.FacetGrid = sns.FacetGrid(
        df,
        col="Tasks",
        row="Workload",
        hue="Library",
        height=3.5,
        aspect=1.1,
        sharey=True,
    )
    g.map_dataframe(
        sns.lineplot, x="Threads", y="Efficiency", marker="o", linewidth=2, markersize=6
    )

    for ax in g.axes.flat:
        ax.axhline(1.0, linestyle="--", color="gray", linewidth=1.3)
        ax.set_xticks(thread_ticks)
        ax.set_ylim(0, 1.15)
        ax.grid(True, linestyle="--", alpha=0.4)

    g.set_axis_labels("Threads (p)", "Efficiency (Speedup / p)")
    g.set_titles(col_template="{col_name} Tasks", row_template="{row_name}")
    g.add_legend(title="Library", frameon=True)
    g.figure.subplots_adjust(top=0.88)
    g.figure.suptitle(
        "Speedup: Parallel Efficiency vs Thread Count", fontsize=14, fontweight="bold"
    )
    output_path: Path = out_dir / "speedup_02_efficiency_curves.png"
    g.savefig(output_path)
    plt.close()
    print(f"Saved plot: {output_path}")


# =========================================================================================
# Latency category: Single-task roundtrip and amortized batch task throughput
# =========================================================================================


def plot_latency_single_task_overhead(df: pd.DataFrame, out_dir: Path) -> None:
    """Latency Plot 1: Roundtrip latency of submitting and immediately retrieving a single task."""
    single_df: pd.DataFrame = df[df["Tasks"] == 1].copy()

    if single_df.empty:
        return

    plt.figure(figsize=(7, 5))
    ax = sns.lineplot(
        data=single_df,
        x="Threads",
        y="Time_us",
        hue="Library",
        style="Library",
        marker="o",
        linewidth=2.2,
        markersize=8,
    )

    thread_ticks: list[int] = sorted(single_df["Threads"].unique())
    ax.set_xticks(thread_ticks)
    ax.set_xlabel("Pool Thread Count", fontsize=11)
    ax.set_ylabel("Roundtrip Overhead (\u03bcs)", fontsize=11)
    ax.set_title(
        "Single-Task Latency Overhead (Submit \u2192 Schedule \u2192 Retrieve)",
        fontsize=13,
        fontweight="bold",
    )
    ax.grid(True, linestyle="--", alpha=0.5)
    output_path: Path = out_dir / "latency_01_single_task_overhead.png"
    plt.savefig(output_path, bbox_inches="tight")
    plt.close()
    print(f"Saved plot: {output_path}")


def plot_latency_batch_amortized_service_time(df: pd.DataFrame, out_dir: Path) -> None:
    """Latency Plot 2: Amortized service time per task across batch volumes."""
    batch_df: pd.DataFrame = df[df["Tasks"] > 1].copy()

    if batch_df.empty:
        return

    # Fallback to computing time per task if custom counter wasn't recorded
    if batch_df["Amortized_us"].isna().all():
        batch_df["Amortized_us"] = (batch_df["Time_us"]) / batch_df["Tasks"]

    batch_df["Tasks_Formatted"] = batch_df["Tasks"].apply(fmt_tasks)
    sorted_tasks: list[int] = sorted(batch_df["Tasks"].unique())
    task_order: list[str] = [fmt_tasks(t) for t in sorted_tasks]

    g: sns.FacetGrid = sns.catplot(
        data=batch_df,
        x="Tasks_Formatted",
        order=task_order,
        y="Amortized_us",
        hue="Library",
        col="Threads",
        kind="bar",
        height=4,
        aspect=1.1,
        sharey=False,
    )

    g.set_axis_labels(
        "Batch Volume (Tasks)", "Amortized Service Time per Task (\u03bcs)"
    )
    g.set_titles(col_template="{col_name} Worker Threads")

    for ax in g.axes.flat:
        ax.grid(True, which="major", axis="y", linestyle="--", alpha=0.5)

    g.figure.subplots_adjust(top=0.82)
    g.figure.suptitle(
        "Amortized Batch Service Time per Task",
        fontsize=13,
        fontweight="bold",
    )

    output_path: Path = out_dir / "latency_02_batch_amortized_service_time.png"
    g.savefig(output_path)
    plt.close()
    print(f"Saved plot: {output_path}")


# =========================================================================================
# Entry point
# =========================================================================================


def main() -> None:
    parser: argparse.ArgumentParser = argparse.ArgumentParser(
        description="Analyze Google Benchmark JSON outputs, routed by benchmark category."
    )
    parser.add_argument(
        "json_file", type=Path, help="Path to Google Benchmark JSON output."
    )
    parser.add_argument(
        "-o",
        "--out-dir",
        type=Path,
        default=Path("plots"),
        help="Directory to save output plots (default: ./plots).",
    )
    args: argparse.Namespace = parser.parse_args()

    # Ensure out_dir is populated properly
    out_dir: Path = args.out_dir if args.out_dir else args.json_file.parent
    out_dir.mkdir(parents=True, exist_ok=True)

    setup_plot_style()

    print(f"Loading benchmark JSON: {args.json_file}")
    df: pd.DataFrame = parse_benchmark_json(args.json_file)

    # Convert category values safely to Title-case for string comparisons
    df["Category_Title"] = df["Category"].astype(str).str.title()
    categories: list[str] = [
        c for c in df["Category_Title"].unique() if c != "Unknown"
    ]
    print(f"Categories found: {categories}")

    if "Comparison" in categories:
        comp_df: pd.DataFrame = df[df["Category_Title"] == "Comparison"]
        print(f"\n[Comparison] Libraries: {comp_df['Library'].unique().tolist()}")
        print(f"[Comparison] Workloads: {comp_df['Workload'].unique().tolist()}")
        pivot_df, other_libs = compute_library_ratios(comp_df)
        plot_comparison_execution_times(comp_df, out_dir)
        plot_comparison_speedup_heatmaps(pivot_df, other_libs, out_dir)
        plot_comparison_thread_scaling(comp_df, out_dir)
        plot_comparison_by_task_size(comp_df, out_dir)
        plot_comparison_performance_ratio(comp_df, other_libs, out_dir)

    if "Speedup" in categories:
        speed_df = df[df["Category_Title"] == "Speedup"]
        print(f"\n[Speedup] Libraries: {speed_df['Library'].unique().tolist()}")
        print(f"[Speedup] Workloads: {speed_df['Workload'].unique().tolist()}")
        speedup_df = compute_parallel_speedup(speed_df)
        plot_speedup_curves(speedup_df, out_dir)
        plot_efficiency_curves(speedup_df, out_dir)

    if "Latency" in categories:
        lat_df = df[df["Category_Title"] == "Latency"]
        print(f"\n[Latency] Libraries: {lat_df['Library'].unique().tolist()}")
        plot_latency_single_task_overhead(lat_df, out_dir)
        plot_latency_batch_amortized_service_time(lat_df, out_dir)

    unhandled: list[str] = [
        c for c in categories if c not in ("Comparison", "Speedup", "Latency")
    ]

    if unhandled:
        print(
            f"\nNote: no analysis routine registered yet for categories {unhandled}. "
            "Add compute_/plot_ functions and a dispatch branch in main() for these."
        )

    print(f"\nAnalysis complete. Plots saved to: {out_dir.resolve()}")


if __name__ == "__main__":
    main()
