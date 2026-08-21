#!/usr/bin/env bash
# Convert a Vivado .bit file into the ZynqMP FPGA Manager .bit.bin format.

set -euo pipefail

BITSTREAM="${1:-}"
OUTPUT="${2:-}"

[[ -n "$BITSTREAM" ]] || {
    echo "Usage: $0 <input.bit> [output.bit.bin]" >&2
    exit 1
}
[[ -f "$BITSTREAM" ]] || {
    echo "ERROR: bitstream not found: $BITSTREAM" >&2
    exit 1
}
[[ "$BITSTREAM" == *.bit ]] || {
    echo "ERROR: input must be a Vivado .bit file" >&2
    exit 1
}

if ! command -v bootgen >/dev/null 2>&1; then
    echo "ERROR: bootgen not found; source scripts/setup-env.sh first" >&2
    exit 1
fi

BITSTREAM="$(realpath "$BITSTREAM")"
OUTPUT="${OUTPUT:-${BITSTREAM}.bin}"
BIF="$(mktemp "${TMPDIR:-/tmp}/kria-bitstream.XXXXXX.bif")"
trap 'rm -f "$BIF"' EXIT

printf 'all:\n{\n  [destination_device=pl] %s\n}\n' "$BITSTREAM" >"$BIF"
bootgen -arch zynqmp -image "$BIF" -w -o "$OUTPUT" >&2

[[ -s "$OUTPUT" ]] || {
    echo "ERROR: bootgen did not produce output: $OUTPUT" >&2
    exit 1
}

echo "$OUTPUT"