#!/usr/bin/env bash
#
# Merges the .profraw files written by train.sh into one .profdata file for
# --fdo_optimize. Uses the llvm-profdata of Envoy's own hermetic LLVM toolchain, so the
# profile format matches the compiler that will read it.
#
# Usage: tools/pgo/merge.sh PROFILE_DIR OUTPUT.profdata
#
# Environment (optional):
#   LLVM_PROFDATA        llvm-profdata to use; skips the toolchain lookup
#   OUTPUT_BASE          Bazel output base to search; default: `bazel info output_base`
#   BAZEL_STARTUP_OPTS   startup options for that `bazel info` call, e.g. "--output_base=/x"
#                        (use the same ones as for the instrumented build)

set -euo pipefail

if [[ $# -ne 2 ]]; then
  sed -n '3,13p' "$0" | sed 's/^# \{0,1\}//'
  exit 2
fi
# Under `bazel run`, resolve relative paths against the directory bazel was started from.
if [[ -n "${BUILD_WORKING_DIRECTORY:-}" ]]; then
  cd "${BUILD_WORKING_DIRECTORY}"
fi
PROFILE_DIR="$1"
OUTPUT="$2"

die() {
  echo "ERROR: $*" >&2
  exit 1
}

# Prints the first x.y.z version number in the tool's --version output.
tool_version() {
  "$1" --version 2>/dev/null | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1 || true
}

PROFILES=("${PROFILE_DIR}"/*.profraw)
[[ -e "${PROFILES[0]}" ]] || die "no .profraw files in ${PROFILE_DIR}"
[[ -e "${OUTPUT}" ]] && die "${OUTPUT} already exists; remove it or pick another name"

if [[ -z "${LLVM_PROFDATA:-}" ]]; then
  if [[ -z "${OUTPUT_BASE:-}" ]]; then
    WORKSPACE="${BUILD_WORKSPACE_DIRECTORY:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
    read -r -a STARTUP <<<"${BAZEL_STARTUP_OPTS:-}"
    OUTPUT_BASE="$(cd "${WORKSPACE}" && bazel "${STARTUP[@]}" info output_base)"
  fi

  # The toolchain repositories often link to one shared LLVM distribution, so compare the
  # resolved files. Exactly one clang version must be present.
  declare -A CLANG_VERSIONS=()
  for candidate in "${OUTPUT_BASE}"/external/*/bin/clang; do
    [[ -x "${candidate}" ]] || continue
    version="$(tool_version "${candidate}")"
    if [[ -n "${version}" ]]; then
      CLANG_VERSIONS["${version}"]+=" $(readlink -f "${candidate}")"
    fi
  done
  [[ ${#CLANG_VERSIONS[@]} -gt 0 ]] ||
    die "no clang under ${OUTPUT_BASE}/external; build the instrumented binary first"
  if [[ ${#CLANG_VERSIONS[@]} -gt 1 ]]; then
    for version in "${!CLANG_VERSIONS[@]}"; do
      echo "  clang ${version}:${CLANG_VERSIONS[${version}]}" >&2
    done
    die "more than one clang version under ${OUTPUT_BASE}/external; set LLVM_PROFDATA"
  fi
  CLANG_VERSION="${!CLANG_VERSIONS[*]}"

  declare -A MATCHES=()
  for candidate in "${OUTPUT_BASE}"/external/*/bin/llvm-profdata; do
    [[ -x "${candidate}" ]] || continue
    [[ "$(tool_version "${candidate}")" == "${CLANG_VERSION}" ]] || continue
    MATCHES["$(readlink -f "${candidate}")"]=1
  done
  [[ ${#MATCHES[@]} -gt 0 ]] ||
    die "no llvm-profdata ${CLANG_VERSION} under ${OUTPUT_BASE}/external; set LLVM_PROFDATA"
  if [[ ${#MATCHES[@]} -gt 1 ]]; then
    printf '  %s\n' "${!MATCHES[@]}" >&2
    die "more than one llvm-profdata ${CLANG_VERSION}; set LLVM_PROFDATA to one of them"
  fi
  LLVM_PROFDATA="${!MATCHES[*]}"
  echo "clang:         ${CLANG_VERSION}"
fi

echo "llvm-profdata: ${LLVM_PROFDATA} ($(tool_version "${LLVM_PROFDATA}"))"
"${LLVM_PROFDATA}" merge -output="${OUTPUT}" "${PROFILES[@]}"
echo "merged ${#PROFILES[@]} files into ${OUTPUT}"
"${LLVM_PROFDATA}" show "${OUTPUT}" | tail -4
