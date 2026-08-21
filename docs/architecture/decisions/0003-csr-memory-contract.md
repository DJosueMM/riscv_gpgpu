# ADR-0003: Canonical CSR and Memory Contract

- **Status**: Proposed
- **Date**: 2026-08-21
- **Decision task**: T098

## Context

The documented `ID/CTRL/STATUS` register block, `fpga_regs.h`, HLS-generated
control registers, and the three AXI-Lite banks in the Vivado design do not
currently describe one proven physical contract. Program, initial-register,
data, and telemetry regions are also represented differently across simulation
and hardware tests.

## Proposed Decision

Define one machine-readable logical contract for:

- capability/version discovery;
- launch, completion, reset, timeout, and fault handling;
- program and initial-context pointers and sizes;
- grid/block geometry and shared-memory requirements;
- data-buffer and result regions;
- counters and trace metadata.

Generate or validate software headers, HLS metadata, Tcl address assignment,
and documentation from that source. Platform shells may translate logical to
physical addresses but may not alter semantics.

## Alternatives

1. Add an RTL wrapper that implements the existing abstract register block.
2. Adopt HLS-generated registers as canonical and generate driver definitions.
3. Keep hand-maintained mirrors. This is rejected because drift already exists.

## Acceptance Evidence

- Driver constants match generated HLS/Vivado metadata.
- Automated tests detect offset, width, access-mode, and reset-value drift.
- A launch traverses the exact documented states on simulated and physical
  hardware.