# ADR-0001: Platform Roles and Portable Accelerator Core

- **Status**: Accepted
- **Date**: 2026-08-20

## Context

The current FPGA flow embeds Kria K26 parts, Zynq PS interfaces, physical DDR
addresses, and `/dev/mem` behavior in multiple layers. The project must use
Kria for bring-up while targeting Alveo U55C without creating two accelerator
architectures.

## Decision

Keep instruction execution, scheduling, SIMT control, barriers, shared memory,
caches, faults, and counters in a board-independent accelerator core.

Implement board-specific shells and host backends:

- Kria: ARM PS control and DDR through AXI HP/HPC;
- Alveo U55C: PCIe host control and HBM through the selected XRT or RTL shell.

SystemC remains the functional reference. HLS/RTL implements the common core,
not a second architectural contract.

## Consequences

- Board addresses and transport APIs cannot appear in the public host API.
- The logical CSR, memory, launch, fault, and trace contracts are shared.
- Kria and Alveo may use different physical mappings and address widths.
- Platform backends require common conformance tests.

## Verification

The same kernel image or validated equivalent, launch packet, input, and result
checker must pass on SystemC, Kria, and Alveo. Platform-specific code must remain
outside the accelerator-core directories.