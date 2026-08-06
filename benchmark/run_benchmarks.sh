#!/usr/bin/env bash
set -euo pipefail

REPETITIONS="${1:-5}"

if ! [[ "${REPETITIONS}" =~ ^[0-9]+$ ]] || [ "${REPETITIONS}" -le 1 ]; then
    echo "  [ERROR] Repetitions must be an integer greater than 1. Got: '${REPETITIONS}'" >&1
    echo "  Usage: $0 [repetitions > 1]" >&1
    exit 1
fi

for cmd in cmake g++ Rscript; do
    if ! command -v "${cmd}" &> /dev/null; then
        echo "  [ERROR] Required command '${cmd}' is not installed or not in PATH." >&2
        exit 1
    fi
done

BENCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BENCH_DIR}/../cmake-build-release"
ANALYSIS_DIR="${BENCH_DIR}/analysis"
SCRIPTS_DIR="${ANALYSIS_DIR}/scripts"
JSON_DIR="${ANALYSIS_DIR}/jsons"
PLOT_DIR="${ANALYSIS_DIR}/plots"

mkdir -p "${JSON_DIR}" "${PLOT_DIR}"

declare -A BENCHMARKS=(
    # ["SortPerformanceBenchmark"]="sort_performance_results.json"
    # ["LatencyBenchmark"]="latency_results.json"
    # ["ComparisonsBenchmark"]="comparisons_results.json"
    # ["SpeedupBenchmark"]="speedup_results.json"
)

BENCH_FLAGS=(
    "--benchmark_repetitions=${REPETITIONS}"
    "--benchmark_report_aggregates_only=true"
    "--benchmark_out_format=json"
)

# TODO: Remove this
CPU_CORES="0,2,4,6"

echo "==> Configuring and Building Release Benchmarks..."
cmake -B "${BUILD_DIR}" -S "${BENCH_DIR}/.." -DCMAKE_BUILD_TYPE=Release

for binary in "${!BENCHMARKS[@]}"; do
    echo "  Building ${binary}..."
    cmake --build "${BUILD_DIR}" --target "${binary}"
    exec_path="${BUILD_DIR}/benchmark/${binary}"
    output_json="${JSON_DIR}/${BENCHMARKS[$binary]}"

    if [[ -f "${exec_path}" ]]; then
        if command -v taskset &> /dev/null && taskset -c "${CPU_CORES}" true 2>/dev/null; then
            echo "  Executing ${binary} pinned to cores ${CPU_CORES}..."
            taskset -c "${CPU_CORES}" "${exec_path}" "${BENCH_FLAGS[@]}" "--benchmark_out=${output_json}"
        else
            echo "  [NOTE] 'taskset -c ${CPU_CORES}' unavailable or invalid on host. Running unpinned."
            "${exec_path}" "${BENCH_FLAGS[@]}" "--benchmark_out=${output_json}"
        fi
    else
        echo "  [WARNING] Binary not found: ${exec_path}. Skipping execution."
    fi
done

echo "==> Running R Analysis via renv..."
cd "${SCRIPTS_DIR}"

for r_script in analyze_*.R; do
    if [[ -f "${r_script}" ]]; then
        echo "  Executing ${r_script}..."
        Rscript "${r_script}"
    fi
done

echo "==> Done. Results and plots stored in ${PLOT_DIR}"
