#!/usr/bin/env bash
# deploy_kria.sh - End-to-end Kria KV260/KR260 deployment (T055)
#
# Cross-compiles the software stack for aarch64, transfers the artifacts and
# the kernel ELF to the Kria board, loads the FPGA bitstream via fpgautil,
# runs the requested test, and produces a pass/fail report.
#
# Usage:
#   scripts/deploy_kria.sh --bitstream <file.bit.bin> --kernel <kernel.elf> --test <test_binary> \
#       --metadata <design.hwh> --expect-marker <exact-text> \
#       [--host <user@kria-ip>] [--report <path>] [--skip-build] [--sudo-pass <password>]
#
# The --sudo-pass option (or KRIA_SUDO_PASS env var) is forwarded to sudo -S
# on the board to allow non-interactive bitstream loading with fpgautil.
#
# Requirements on the build host: aarch64-linux-gnu-g++ toolchain, cmake, ssh/scp.
# Requirements on the Kria board:  fpgautil, an accessible SSH account (default: ubuntu@kria).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

BITSTREAM=""
KERNEL=""
TEST_BIN=""
METADATA=""
KRIA_HOST="${KRIA_HOST:-ubuntu@kria}"
REPORT="${REPO_ROOT}/docs/verification/kria_results.md"
SKIP_BUILD=0
BUILD_DIR="${REPO_ROOT}/build-kria-aarch64"
REMOTE_DIR="/home/${KRIA_HOST%%@*}/riscv_gpgpu_deploy"
KRIA_SUDO_PASS="${KRIA_SUDO_PASS:-}"
EXPECT_MARKER=""

usage() { grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 1; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --bitstream) BITSTREAM="$2"; shift 2 ;;
        --kernel)    KERNEL="$2"; shift 2 ;;
        --test)      TEST_BIN="$2"; shift 2 ;;
        --metadata)  METADATA="$2"; shift 2 ;;
        --host)      KRIA_HOST="$2"; shift 2 ;;
        --report)    REPORT="$2"; shift 2 ;;
        --expect-marker) EXPECT_MARKER="$2"; shift 2 ;;
        --skip-build) SKIP_BUILD=1; shift ;;
        --sudo-pass) KRIA_SUDO_PASS="$2"; shift 2 ;;
        -h|--help)   usage ;;
        *) echo "Unknown argument: $1" >&2; usage ;;
    esac
done

[[ -n "$BITSTREAM" && -n "$KERNEL" && -n "$TEST_BIN" && -n "$METADATA" && -n "$EXPECT_MARKER" ]] || {
    echo "ERROR: --bitstream, --kernel, --test, --metadata, and --expect-marker are required." >&2; usage; }
[[ -f "$BITSTREAM" ]] || { echo "ERROR: bitstream not found: $BITSTREAM" >&2; exit 1; }
[[ -f "$KERNEL" ]]    || { echo "ERROR: kernel ELF not found: $KERNEL" >&2; exit 1; }
[[ -f "$METADATA" ]]  || { echo "ERROR: hardware metadata not found: $METADATA" >&2; exit 1; }

log() { echo "[deploy_kria] $*"; }

# ── 1. Cross-compile the software stack for aarch64 ─────────────────────────
if [[ "$SKIP_BUILD" -eq 0 ]]; then
    command -v aarch64-linux-gnu-g++ >/dev/null || {
        echo "ERROR: aarch64-linux-gnu-g++ not found. Install gcc-aarch64-linux-gnu." >&2; exit 1; }
    log "Cross-compiling for aarch64 into ${BUILD_DIR}"
    cmake -S "$REPO_ROOT" -B "$BUILD_DIR" \
        -DCMAKE_TOOLCHAIN_FILE="${REPO_ROOT}/fpga/toolchain-aarch64.cmake" \
        -DFPGA_TARGET=ON \
        -DBUILD_SYSTEMC_MODELS=OFF \
        -DBUILD_SYSTEMC_INTEGRATION=OFF \
        -DBUILD_BENCHMARKS=OFF \
        -DBUILD_TESTS=OFF
    cmake --build "$BUILD_DIR" -j"$(nproc)"
fi

TEST_PATH="${BUILD_DIR}/bin/${TEST_BIN}"
[[ -f "$TEST_PATH" ]] || TEST_PATH="$TEST_BIN"
[[ -f "$TEST_PATH" ]] || { echo "ERROR: test binary not found: $TEST_BIN" >&2; exit 1; }

# ── 2. Transfer artifacts to the board ───────────────────────────────────────
log "Transferring artifacts to ${KRIA_HOST}:${REMOTE_DIR}"
ssh "$KRIA_HOST" "mkdir -p '$REMOTE_DIR'"
REMOTE_METADATA="$(ssh "$KRIA_HOST" '
    model=$(tr -d "\000" </sys/firmware/devicetree/base/model 2>/dev/null || printf unknown)
    printf "Model: %s\n" "$model"
    printf "Kernel: %s\n" "$(uname -a)"
    printf "Command line: %s\n" "$(cat /proc/cmdline)"
')"
scp "$BITSTREAM" "$KERNEL" "$TEST_PATH" "$METADATA" "$KRIA_HOST:$REMOTE_DIR/"

BITSTREAM_NAME="$(basename "$BITSTREAM")"
KERNEL_NAME="$(basename "$KERNEL")"
TEST_NAME="$(basename "$TEST_PATH")"
METADATA_NAME="$(basename "$METADATA")"
BITSTREAM_SHA256="$(sha256sum "$BITSTREAM" | awk '{print $1}')"
KERNEL_SHA256="$(sha256sum "$KERNEL" | awk '{print $1}')"
TEST_SHA256="$(sha256sum "$TEST_PATH" | awk '{print $1}')"
METADATA_SHA256="$(sha256sum "$METADATA" | awk '{print $1}')"
SOURCE_COMMIT="$(git -C "$REPO_ROOT" rev-parse HEAD 2>/dev/null || printf unknown)"
if [[ -n "$(git -C "$REPO_ROOT" status --porcelain 2>/dev/null)" ]]; then
    SOURCE_STATE=DIRTY
else
    SOURCE_STATE=CLEAN
fi

# ── 3. Load the bitstream and run the test on the board ─────────────────────
log "Loading bitstream and running ${TEST_NAME} on the board"
RUN_LOG="$(mktemp "${TMPDIR:-/tmp}/kria_run.XXXXXX.log")"
STATUS=PASS
LOG_ALREADY_PRINTED=0
if [[ -n "$KRIA_SUDO_PASS" ]]; then
    if ! ssh "$KRIA_HOST" bash -s -- \
            "$REMOTE_DIR" "$BITSTREAM_NAME" "$KERNEL_NAME" "$TEST_NAME" "$KRIA_SUDO_PASS" <<'REMOTE' >"$RUN_LOG" 2>&1
set -euo pipefail
REMOTE_DIR="$1"; BITSTREAM="$2"; KERNEL="$3"; TEST="$4"; SUDO_PASS="$5"
cd "$REMOTE_DIR"
echo "$SUDO_PASS" | sudo -S fpgautil -b "$BITSTREAM"
# Allow time for the PLL to lock and proc_sys_reset to release ap_rst_n.
sleep 2
chmod +x "$TEST"
echo "$SUDO_PASS" | sudo -S env GPGPU_KERNEL_ELF="$REMOTE_DIR/$KERNEL" "./$TEST"
REMOTE
    then
        STATUS=FAIL
    fi
elif [[ -t 0 ]]; then
    printf -v REMOTE_COMMAND \
        'set -euo pipefail; cd %q; sudo fpgautil -b %q; sleep 2; chmod +x %q; sudo env GPGPU_KERNEL_ELF=%q ./%q' \
        "$REMOTE_DIR" "$BITSTREAM_NAME" "$TEST_NAME" \
        "$REMOTE_DIR/$KERNEL_NAME" "$TEST_NAME"
    LOG_ALREADY_PRINTED=1
    if ! ssh -tt "$KRIA_HOST" "$REMOTE_COMMAND" 2>&1 | tee "$RUN_LOG"; then
        STATUS=FAIL
    fi
else
    echo "ERROR: sudo requires a TTY; run interactively or set KRIA_SUDO_PASS" >"$RUN_LOG"
    STATUS=FAIL
fi
if [[ "$STATUS" == PASS ]] && ! grep -Fq -- "$EXPECT_MARKER" "$RUN_LOG"; then
    echo "ERROR: expected success marker not found: ${EXPECT_MARKER}" >>"$RUN_LOG"
    STATUS=FAIL
fi
if [[ "$LOG_ALREADY_PRINTED" -eq 0 ]]; then
    cat "$RUN_LOG"
fi
if [[ "$STATUS" == PASS ]]; then
    REPORT_RESULT="VALIDATED ON HARDWARE"
else
    REPORT_RESULT="FAILED"
fi

# ── 4. Emit pass/fail report ──────────────────────────────────────────────────
mkdir -p "$(dirname "$REPORT")"
{
    echo "# Kria Deployment Report"
    echo
    echo "- Date: $(date -u '+%Y-%m-%d %H:%M UTC')"
    echo "- Source commit: \`${SOURCE_COMMIT}\`"
    echo "- Source tree: ${SOURCE_STATE}"
    echo "- Board: ${KRIA_HOST}"
    while IFS= read -r metadata; do
        echo "- ${metadata}"
    done <<<"${REMOTE_METADATA}"
    echo "- Bitstream: ${BITSTREAM_NAME}"
    echo "- Bitstream SHA-256: \`${BITSTREAM_SHA256}\`"
    echo "- Kernel ELF: ${KERNEL_NAME}"
    echo "- Kernel SHA-256: \`${KERNEL_SHA256}\`"
    echo "- Test: ${TEST_NAME}"
    echo "- Test SHA-256: \`${TEST_SHA256}\`"
    echo "- Hardware metadata: ${METADATA_NAME}"
    echo "- Hardware metadata SHA-256: \`${METADATA_SHA256}\`"
    echo "- Required marker: \`${EXPECT_MARKER}\`"
    echo "- Result: **${REPORT_RESULT}**"
    echo
    echo "## Test output"
    echo
    echo '```'
    cat "$RUN_LOG"
    echo '```'
} > "$REPORT"
rm -f "$RUN_LOG"

log "Report written to ${REPORT}"
log "Result: ${REPORT_RESULT}"
[[ "$STATUS" == PASS ]]
