# ADR-0004: Kria Memory Transport

- **Status**: Proposed
- **Date**: 2026-08-20
- **Decision task**: T107

## Context

The current hardware smoke report reaches AXI-Lite but cannot map the reserved
DDR aperture, so it skips kernel execution. OCM is useful for diagnostics but is
too small to represent the intended external-memory system. Permissive
`/dev/mem` behavior is not a robust deployment contract.

## Proposed Decision

Use reserved PS DDR exposed through UIO/CMA/dma-buf or an equivalent controlled
Linux interface as the canonical Kria program and data transport. Document and
implement cache maintenance, ownership, alignment, address translation, and
failure behavior. Retain OCM only as an explicitly labeled bring-up path.

## Acceptance Evidence

- The board reserves and exposes the declared region reproducibly.
- H2D and D2H transfers are coherent without relying on accidental cache state.
- Kernel ELF, initial contexts, and data buffers coexist without overlap.
- `vector_add` and SAXPY pass after a complete load/run/readback sequence.
- A blocked DDR mapping or skipped kernel is a failed hardware gate.