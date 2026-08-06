#!/usr/bin/env bash
set -euo pipefail

for cmd in cmake g++; do
    if ! command -v "${cmd}" &> /dev/null; then
        echo "  [ERROR] Required command '${cmd}' is not installed or not in PATH." >&2
        exit 1
    fi
done

TEST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${TEST_DIR}/../cmake-build-debug"

echo "==> Configuring Debug Build..."
cmake -B "${BUILD_DIR}" -S "${TEST_DIR}/.." -DCMAKE_BUILD_TYPE=Debug

for test_cpp in *Tests.cpp; do
    test_name="${test_cpp%.cpp}"
    echo "  Building ${test_name}..."
    cmake --build "${BUILD_DIR}" --target "${test_name}"
    exec_path="${BUILD_DIR}/tests/${test_name}"

    if [[ -f "${exec_path}" ]]; then
        echo "  Executing ${test_name}..."
        "${exec_path}" --gtest_filter=* --gtest_color=no
    else
        echo "  [WARNING] Test binary not found: ${exec_path}. Skipping execution."
    fi
done

echo "==> Test suite complete."
