# ADR-0005: Alveo U55C Shell Selection

- **Status**: Proposed
- **Date**: 2026-08-20
- **Decision tasks**: T112-T113

## Context

Alveo U55C was previously removed from scope because its platform support was
not installed. It is now the intended final target. The project has not yet
shown whether a Vitis acceleration/XRT shell or an RTL kernel gives the best
balance of control, HBM access, reproducibility, and maintenance.

## Proposed Evaluation

Build two minimal, timeboxed prototypes around the same accelerator-core and
logical launch contract:

1. Vitis acceleration with XRT/xclbin;
2. RTL kernel integration.

Compare HBM bank mapping, PCIe transfers, interrupts, emulation, profiling,
timing closure, packaging, build/debug time, host API fit, and long-term
maintenance. Select one route and stop developing the other.

## Acceptance Evidence

- Both prototypes execute a bounded transfer/launch/completion smoke test or
  document a concrete blocker.
- Measurements and engineering effort are recorded using the same checklist.
- The selected route supports 64-bit device addresses and explicit HBM bank
  placement.
- A follow-up ADR revision records the selected route and rejected alternative.