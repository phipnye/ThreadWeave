#!/usr/bin/env bash
set -euo pipefail

REPETITIONS="${1:-5}"

if ! [[ "${REPETITIONS}" =~ ^[0-9]+$ ]] || [ "${REPETITIONS}" -le 1 ]; then
    echo "[ERROR] Repetitions must be an integer greater than 1. Got: '${REPETITIONS}'" >&2
    echo "Usage: $0 [repetitions > 1]" >&2
    exit 1
fi

for cmd in cmake Rscript; do
    if ! command -v "${cmd}" &> /dev/null; then
        echo "[ERROR] Required command '${cmd}' is not installed or not in PATH." >&2
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
    ["SortPerformanceBenchmark"]="sort_performance_results.json"
    ["LatencyBenchmark"]="latency_results.json"
    ["ComparisonsBenchmark"]="comparisons_results.json"
    ["SpeedupBenchmark"]="speedup_results.json"
)

BENCH_FLAGS=(
    "--benchmark_repetitions=${REPETITIONS}"
    "--benchmark_report_aggregates_only=true"
    "--benchmark_out_format=json"
)

PRIMARY_PCORES="0,2,4,6"
SMT_SIBLINGS=(1 3 5 7)
ORIG_ASLR=""
ORIG_TURBO=""
declare -A ORIG_GOVERNORS
declare -A ORIG_SMT_STATE

restore_system_state() {
    # Restore ASLR
    if [[ -n "${ORIG_ASLR}" ]]; then
        echo "${ORIG_ASLR}" | sudo tee /proc/sys/kernel/randomize_va_space > /dev/null
        echo "[RESTORED] ASLR"
    fi

    # Restore turbo boost
    if [[ -n "${ORIG_TURBO}" && -f /sys/devices/system/cpu/intel_pstate/no_turbo ]]; then
        echo "${ORIG_TURBO}" | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo > /dev/null
        echo "[RESTORED] Turbo Boost"
    elif [[ -n "${ORIG_TURBO}" && -f /sys/devices/system/cpu/cpufreq/boost ]]; then
        echo "${ORIG_TURBO}" | sudo tee /sys/devices/system/cpu/cpufreq/boost > /dev/null
        echo "[RESTORED] Turbo Boost"
    fi

    # Restore SMT siblings
    if [[ ${#ORIG_SMT_STATE[@]} -gt 0 ]]; then
        for cpu_id in "${!ORIG_SMT_STATE[@]}"; do
            path="/sys/devices/system/cpu/cpu${cpu_id}/online"
            if [[ -f "${path}" ]]; then
                echo "${ORIG_SMT_STATE[${cpu_id}]}" | sudo tee "${path}" > /dev/null
            fi
        done
        echo "[RESTORED] SMT siblings state (CPUs: ${SMT_SIBLINGS[*]})"
    fi

    # Restore governors
    if [[ ${#ORIG_GOVERNORS[@]} -gt 0 ]]; then
        for cpu_gov in "${!ORIG_GOVERNORS[@]}"; do
            if [[ -f "${cpu_gov}" ]]; then
                echo "${ORIG_GOVERNORS[${cpu_gov}]}" | sudo tee "${cpu_gov}" > /dev/null
            fi
        done
        echo "[RESTORED] CPU scaling governors"
    fi
}

trap restore_system_state EXIT INT TERM

configure_system_for_benchmarking() {
    # Disable ASLR
    if [[ -f /proc/sys/kernel/randomize_va_space ]]; then
        ORIG_ASLR=$(cat /proc/sys/kernel/randomize_va_space)
        echo 0 | sudo tee /proc/sys/kernel/randomize_va_space > /dev/null
        echo "[DISABLED] ASLR"
    fi

    # Disable turbo boost
    if [[ -f /sys/devices/system/cpu/intel_pstate/no_turbo ]]; then
        ORIG_TURBO=$(cat /sys/devices/system/cpu/intel_pstate/no_turbo)
        echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo > /dev/null
        echo "[DISABLED] Turbo Boost"
    elif [[ -f /sys/devices/system/cpu/cpufreq/boost ]]; then
        ORIG_TURBO=$(cat /sys/devices/system/cpu/cpufreq/boost)
        echo 0 | sudo tee /sys/devices/system/cpu/cpufreq/boost > /dev/null
        echo "[DISABLED] Turbo Boost"
    fi

    # Set governors to performance
    for gov in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
        if [[ -f "${gov}" ]]; then
            cpu_online_path="$(dirname "$(dirname "${gov}")")/online"
            if [[ -f "${cpu_online_path}" ]] && [[ "$(cat "${cpu_online_path}")" -eq 0 ]]; then
                continue
            fi

            ORIG_GOVERNORS["${gov}"]=$(cat "${gov}")
            echo performance | sudo tee "${gov}" > /dev/null
        fi
    done
    echo "[SET] CPU governors to 'performance'"

    # Disable SMT siblings
    for cpu_id in "${SMT_SIBLINGS[@]}"; do
        path="/sys/devices/system/cpu/cpu${cpu_id}/online"
        if [[ -f "${path}" ]]; then
            ORIG_SMT_STATE["${cpu_id}"]=$(cat "${path}")
            echo 0 | sudo tee "${path}" > /dev/null
        fi
    done
    echo "[OFFLINE] SMT hyperthreads (CPUs: ${SMT_SIBLINGS[*]})"
}

configure_system_for_benchmarking
cmake -B "${BUILD_DIR}" -S "${BENCH_DIR}/.." -DCMAKE_BUILD_TYPE=Release

for binary in "${!BENCHMARKS[@]}"; do
    echo "==> Building ${binary}..."
    cmake --build "${BUILD_DIR}" --target "${binary}"
    exec_path="${BUILD_DIR}/benchmark/${binary}"
    output_json="${JSON_DIR}/${BENCHMARKS[$binary]}"

    if [[ -f "${exec_path}" ]]; then
        echo "[RUNNING] ${binary} pinned to CPUs ${PRIMARY_PCORES}..."
        sudo chrt -f 80 taskset -c "${PRIMARY_PCORES}" "${exec_path}" "${BENCH_FLAGS[@]}" "--benchmark_out=${output_json}"
    else
        echo "[WARNING] Binary not found: ${exec_path}. Skipping execution."
    fi
done

echo "==> Running R Analysis..."
cd "${SCRIPTS_DIR}"

for r_script in analyze_*.R; do
    if [[ -f "${r_script}" ]]; then
        echo "Executing ${r_script}..."
        Rscript "${r_script}"
    fi
done

echo "==> Done. Results and plots stored in ${PLOT_DIR}"
