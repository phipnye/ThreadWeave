#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/../cmake-build-release"
ANALYSIS_DIR="${SCRIPT_DIR}/analysis"
JSON_DIR="${ANALYSIS_DIR}/jsons"
PLOT_DIR="${ANALYSIS_DIR}/plots"

mkdir -p "${JSON_DIR}" "${PLOT_DIR}"

declare -A BENCHMARKS=(
    ["SortPerformanceBenchmark"]="sort_performance_results.json"
#    ["LatencyBenchmark"]="latency_results.json"
#    ["ComparisonsBenchmark"]="comparisons_results.json"
#    ["SpeedupBenchmark"]="speedup_results.json"
)

BENCH_FLAGS=(
    "--benchmark_repetitions=10"
    "--benchmark_report_aggregates_only=true"
    "--benchmark_out_format=json"
)

CPU_CORES="0,2,4,6"

for binary in "${!BENCHMARKS[@]}"; do
    exec_path="${BUILD_DIR}/benchmark/${binary}"
    output_json="${JSON_DIR}/${BENCHMARKS[$binary]}"

    if [[ -f "${exec_path}" ]]; then
        taskset -c "${CPU_CORES}" "${exec_path}" "${BENCH_FLAGS[@]}" "--benchmark_out=${output_json}"
    else
        echo "  [WARNING] Binary not found: ${exec_path}. Skipping."
    fi
done

if command -v Rscript &> /dev/null; then
    cd "${ANALYSIS_DIR}"/scripts

    for r_script in analyze_*.R; do
        if [[ -f "${r_script}" ]]; then
            Rscript "${r_script}"
        fi
    done
else
    echo "  [ERROR] Rscript not found in PATH. Skipping plotting."
    exit 1
fi
