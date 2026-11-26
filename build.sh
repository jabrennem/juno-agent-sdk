#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${JUNO_AGENT_BUILD_DIR:-${ROOT_DIR}/build}"
BUILD_TYPE="${JUNO_AGENT_BUILD_TYPE:-Release}"
CMAKE_BIN="${CMAKE_BIN:-cmake}"
CTEST_BIN="${CTEST_BIN:-ctest}"

build_examples=OFF
build_tests=OFF
cmake_args=(
  "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}"
  "-DJUNO_AGENT_ENABLE_LLAMA_CPP=${JUNO_AGENT_ENABLE_LLAMA_CPP:-ON}"
  "-DJUNO_AGENT_BUILD_LLAMA_SERVER=${JUNO_AGENT_BUILD_LLAMA_SERVER:-OFF}"
)

while (( $# > 0 )); do
  case "$1" in
    --examples) build_examples=ON ;;
    --test) build_tests=ON ;;
    -h|--help)
      cat <<'EOF'
Usage: ./build.sh [--examples] [--test] [CMake options]

Build the SDK in build/.

  --examples  Also build the Weather Agent example.
  --test      Also build and run the SDK tests.
  --help      Show this help.
EOF
      exit 0
      ;;
    --)
      shift
      cmake_args+=("$@")
      break
      ;;
    *) cmake_args+=("$1") ;;
  esac
  shift
done

"${CMAKE_BIN}" -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
  "${cmake_args[@]}" \
  -DJUNO_AGENT_BUILD_TESTS="${build_tests}" \
  -DJUNO_AGENT_BUILD_EXAMPLES="${build_examples}"

"${CMAKE_BIN}" --build "${BUILD_DIR}" --parallel

if [[ "${build_tests}" == ON ]]; then
  "${CTEST_BIN}" --test-dir "${BUILD_DIR}" --output-on-failure
fi

printf '\nBuilt SDK in %s\n' "${BUILD_DIR}"
if [[ "${build_examples}" == ON ]]; then
  printf 'Built Weather Agent example: %s/juno_weather_agent\n' "${BUILD_DIR}"
fi
if [[ "${build_tests}" == ON ]]; then
  printf 'SDK tests passed\n'
fi
