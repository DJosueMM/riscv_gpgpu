#!/usr/bin/env bash
# Build and run the minimal PL-written sentinel proof on Kria.
#
# Usage:
#   scripts/run_kria_first_kernel.sh --bitstream <file.bit|file.bit.bin> \
#       --hwh <design.hwh> [--memory ddr|ocm] [--host ubuntu@kria] \
#       [--report <path>] [--skip-build]

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${KRIA_BUILD_DIR:-${REPO_ROOT}/build-kria-first-aarch64}"
BITSTREAM=""
HWH=""
MEMORY="ddr"
KRIA_HOST="${KRIA_HOST:-ubuntu@kria}"
REPORT="${REPO_ROOT}/docs/verification/kria_results.md"
SKIP_BUILD=0

usage() {
    grep '^#' "$0" | sed 's/^# \{0,1\}//'
    exit 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --bitstream) BITSTREAM="$2"; shift 2 ;;
        --hwh) HWH="$2"; shift 2 ;;
        --memory) MEMORY="$2"; shift 2 ;;
        --host) KRIA_HOST="$2"; shift 2 ;;
        --report) REPORT="$2"; shift 2 ;;
        --skip-build) SKIP_BUILD=1; shift ;;
        -h|--help) usage ;;
        *) echo "ERROR: unknown argument: $1" >&2; usage ;;
    esac
done

[[ -n "$BITSTREAM" ]] || { echo "ERROR: --bitstream is required" >&2; usage; }
[[ -n "$HWH" ]] || { echo "ERROR: --hwh is required" >&2; usage; }
[[ -f "$BITSTREAM" ]] || { echo "ERROR: bitstream not found: $BITSTREAM" >&2; exit 1; }
[[ -f "$HWH" ]] || { echo "ERROR: HWH not found: $HWH" >&2; exit 1; }
[[ "$MEMORY" == ocm || "$MEMORY" == ddr ]] || {
    echo "ERROR: --memory must be ocm or ddr" >&2
    exit 1
}

python3 "${REPO_ROOT}/scripts/validate_kria_metadata.py" "$HWH" --memory "$MEMORY"

if [[ "$BITSTREAM" == *.bit ]]; then
    BITSTREAM="$("${REPO_ROOT}/scripts/package_kria_bitstream.sh" "$BITSTREAM")"
fi

if [[ "$SKIP_BUILD" -eq 0 ]]; then
    cmake -S "$REPO_ROOT" -B "$BUILD_DIR" \
        -DCMAKE_TOOLCHAIN_FILE="${REPO_ROOT}/fpga/toolchain-aarch64.cmake" \
        -DFPGA_TARGET=ON \
        -DBUILD_SYSTEMC_MODELS=OFF \
        -DBUILD_SYSTEMC_INTEGRATION=OFF \
        -DBUILD_BENCHMARKS=OFF \
        -DBUILD_TESTS=ON
    cmake --build "$BUILD_DIR" \
        --target kria_first_kernel_test "kria_sentinel_${MEMORY}_kernel" \
        -j"$(nproc)"
fi

RUNNER="${BUILD_DIR}/bin/kria_first_kernel_test"
KERNEL="${BUILD_DIR}/kernels/kria_sentinel_${MEMORY}.elf"
[[ -f "$RUNNER" ]] || { echo "ERROR: runner not found: $RUNNER" >&2; exit 1; }
[[ -f "$KERNEL" ]] || { echo "ERROR: kernel not found: $KERNEL" >&2; exit 1; }

exec "${REPO_ROOT}/scripts/deploy_kria.sh" \
    --skip-build \
    --bitstream "$BITSTREAM" \
    --kernel "$KERNEL" \
    --test "$RUNNER" \
    --metadata "$HWH" \
    --host "$KRIA_HOST" \
    --report "$REPORT" \
    --expect-marker "PASS: KRIA_FIRST_KERNEL"