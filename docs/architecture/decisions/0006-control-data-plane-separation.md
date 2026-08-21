# ADR-0006: Control, Data, and Trace Plane Separation

- **Status**: Accepted
- **Date**: 2026-08-20
- **Decision tasks**: T096b-T096d, T098, T101

## Context

Asking whether AXI4-Lite is the best interface has no single answer because
control, command submission, bulk memory, internal requests, and trace traffic
have different rate, ordering, and bandwidth requirements. The current HLS
design already uses AXI4-Lite for scalar arguments and full AXI4 masters for
memory, but the generated control banks, abstract CSR map, and performance
limits are not yet reconciled.

The current runtime memory path also allows one outstanding request per CU,
routes all CUs through an N:1 arbiter and one memory pipeline, and then reaches
one Kria HPC port. Increasing AXI width or outstanding pragmas alone cannot
guarantee throughput if the source protocol remains serialized.

## Decision

Separate the platform interface into these planes:

1. Use AXI4-Lite for version/capability discovery, bounded launch metadata,
   doorbells, completion, faults, interrupt control, and snapshot counters.
2. Use full AXI4 memory-mapped interfaces and managed buffers/DMA for program
   images, contexts, kernel data, and results.
3. Use ready/valid streams for internal request/response traffic. Add tags,
   credits, queues, coalescing, or independent ports only when T096c-T096d
   measurements justify the complexity.
4. Send high-volume trace to a memory-backed ring or streaming DMA path, never
   through AXI4-Lite registers.
5. Keep direct CSR launch as the simple baseline. T096b shall replace or
   complement it with a descriptor queue and AXI4-Lite doorbell if measured
   launch/control overhead exceeds 1% of end-to-end time for a target workload
   or fails its required launch rate.

The logical plane semantics are portable. Kria AXI HP/HPC choices and Alveo HBM
ports remain platform-shell decisions.

## Consequences

- AXI4-Lite width or register count is not treated as a kernel-throughput knob.
- T098 must reconcile the HLS-generated control banks with one logical CSR
  contract, using generation or a real wrapper rather than hand-maintained
  assumptions.
- T105 must report where requests serialize and how much of the measured
  memory ceiling is reached before adding CUs or widening interfaces.
- U55C may expose multiple full AXI/HBM ports without changing the host API or
  accelerator execution semantics.
- Counter snapshots need a coherent latch/read protocol; bulk traces need
  explicit bandwidth and perturbation measurements.

## Verification

- Direct CSR and queued launch alternatives are measured with T096b's
  microkernel and representative-workload matrix.
- AXI width, burst, outstanding depth, port count, and request concurrency are
  swept independently under T096c-T096d.
- AXI protocol checks prove ordering, backpressure, timeout, fault, and reset
  behavior.
- Physical reports separate control, H2D, kernel, D2H, and trace overhead.