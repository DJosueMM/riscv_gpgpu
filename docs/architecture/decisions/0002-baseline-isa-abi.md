# ADR-0002: Baseline ISA and Kernel ABI

- **Status**: Accepted
- **Date**: 2026-08-21

## Context

The repository contains a virtual ISA path, RV32 binary execution, a PTX
transpiler, and HLS custom instructions. Requiring a production LLVM target or
RVV before physical validation would delay the hardware contract while leaving
the current binary path ambiguous.

## Decision

The first physical end-to-end baseline uses:

- RV32IMF, little-endian;
- no compressed instructions;
- the minimum documented custom SIMT operations required for divergence,
  barriers, and thread context;
- a stable ELF/program-image contract;
- explicit launch geometry, argument, stack, return, shared-memory, and fault
  semantics.

The full LLVM target, additional ISA extensions, and RVV are later work and do
not block the first Kria or Alveo correctness gates.

## Consequences

- An ELF validator must reject unsupported instructions, relocations, address
  widths, compressed code, and oversized program images before deployment.
- SystemC and HLS decode behavior must share conformance vectors.
- ABI or opcode changes require a new ADR and compatibility note.

## Verification

Use the same ISA vectors and representative ELF kernels for decoder tests,
SystemC/HLS parity, and physical result checks. Integer state is bit-exact;
floating-point tolerances are declared per test.