#!/usr/bin/env bash
set -euo pipefail

TEST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${TEST_DIR}/../cmake-build-debug"

cmake -B "${BUILD_DIR}" -S "${TEST_DIR}/.." -DCMAKE_BUILD_TYPE=Debug

for test_cpp in *Tests.cpp; do
  test_name="${test_cpp%.cpp}"
  echo "   Building ${test_name} ..."
  cmake --build "${BUILD_DIR}" --target "${test_name}"
  exec_path="${BUILD_DIR}/tests/${test_name}"
  "${exec_path}" --gtest_filter=* --gtest_color=no
done
