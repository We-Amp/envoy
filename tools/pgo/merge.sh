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
  sed -n '3,15p' "$0" | sed 's/^# \{0,1\}//'
  exit 2
fi
PROFILE_DIR="$1"
OUTPUT="$2"

die() {
  echo "ERROR: $*" >&2
  exit 1
}

PROFILES=("${PROFILE_DIR}"/*.profraw)
[[ -e "${PROFILES[0]}" ]] || die "no .profraw files in ${PROFILE_DIR}"
[[ -e "${OUTPUT}" ]] && die "${OUTPUT} already exists; remove it or pick another name"

if [[ -z "${LLVM_PROFDATA:-}" ]]; then
  if [[ -z "${OUTPUT_BASE:-}" ]]; then
    WORKSPACE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
    read -r -a STARTUP <<<"${BAZEL_STARTUP_OPTS:-}"
    OUTPUT_BASE="$(cd "${WORKSPACE}" && bazel "${STARTUP[@]}" info output_base)"
  fi
  for candidate in "${OUTPUT_BASE}"/external/*/bin/llvm-profdata; do
    if [[ -x "${candidate}" ]] && "${candidate}" --version >/dev/null 2>&1; then
      LLVM_PROFDATA="${candidate}"
      break
    fi
  done
  [[ -n "${LLVM_PROFDATA:-}" ]] ||
    die "no llvm-profdata under ${OUTPUT_BASE}/external; build the instrumented binary first"
fi

echo "llvm-profdata: ${LLVM_PROFDATA} ($("${LLVM_PROFDATA}" --version | grep -m1 -i 'llvm version'))"
CLANG="$(dirname "${LLVM_PROFDATA}")/clang"
if [[ -x "${CLANG}" ]]; then
  echo "clang:         $("${CLANG}" --version | head -1)"
fi

"${LLVM_PROFDATA}" merge -output="${OUTPUT}" "${PROFILES[@]}"
echo "merged ${#PROFILES[@]} files into ${OUTPUT}"
"${LLVM_PROFDATA}" show "${OUTPUT}" | tail -4
