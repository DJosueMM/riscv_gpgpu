# Kria First-Kernel Vertical Slice

**Status**: Active execution plan
**Branch**: `feat/kria-first-kernel`
**Parent**: `docs/fpga-architecture-roadmap`
**Date**: 2026-08-20
**Roadmap mapping**: T106-T110; the sentinel proof enables T109

## 1. Objective

Prove that the current RISC-V GPGPU can execute a real instruction stream in
the Kria PL and produce a host-verifiable memory result. This branch optimizes
for the shortest honest physical proof, not for final driver, ABI, performance,
or interface compliance.

The branch succeeds only when a kernel writes a known signature through the PL
memory path and the ARM host reads back the expected value. Loading a bitstream,
accessing AXI4-Lite, observing `done`, or skipping a prerequisite is not enough.

## 2. Current State

| Capability | State | Evidence or gap |
|---|---|---|
| HLS IP export | Working | Both scheduler and memory IP export with Vitis 2026.1 |
| KV260 synthesis/implementation/bitstream | Working | Batch Vivado flow completes |
| Bitstream load | Working | `fpgautil` loaded the prior image on the board |
| Scheduler AXI4-Lite access | Working | Physical reads/writes reached `0xA0000000` |
| Shared host/PL memory | DDR addressed, not physically proven | Generated HWH maps DDR low to every PL master; OCM is explicitly excluded in the current bitstream |
| Scheduler physical register contract | Checked before deployment | HWH bases, offsets, widths, and selected memory path are validated fail-closed |
| Memory-pipeline start/base configuration | Implemented, physical proof pending | The first-kernel runner configures and starts control bank `0xA0020000` |
| Kernel launch to `done` | Code exists, physical evidence incomplete | `fpga_rodinia_bench` loads ELF and polls status but validates no output |
| Kernel result readback | Implemented, physical proof pending | The first-kernel runner poisons and verifies an exact PL-written signature |

The existing `fpga_smoke_test` cannot prove execution: it returns success when
DDR mapping fails, and its DDR-success branch still contains a kernel-launch
TODO. The existing `fpga_rodinia_bench` is a useful launcher prototype, but a
`done` bit without a result oracle is not a correctness gate.

## 3. Definition of First Physical PASS

One run must perform every step below without a skip:

1. Load a bitstream whose SHA-256 and Vivado/HLS metadata are recorded.
2. Read the three physical HLS control banks at their generated addresses.
3. Map one host/PL-visible memory region and verify host write/read access.
4. Fill the result word with a poison value that differs from the expected
   signature.
5. Load a validated RV32IMF ELF into the selected region and read back its
   bytes before launch.
6. Configure and start `memory_pipeline`, including its generated
   `global_mem` base-pointer register.
7. Configure scheduler pointer, length, warp, and start registers from generated
   metadata rather than an abstract hand-written map.
8. Observe a real launch transition, then `done=1`, `fault=0`, and no timeout.
9. Read the result word after completion and compare it exactly to the expected
   signature.
10. Emit nonzero exit status for any mapping failure, skipped step, timeout,
    fault, unchanged poison value, or mismatch.

The required terminal line is structurally equivalent to:

```text
PASS: KRIA_FIRST_KERNEL expected=0x47504750 actual=0x47504750 done=1 fault=0
```

## 4. Sentinel Kernel

Use a dedicated kernel before vector add or Rodinia. It has one externally
visible effect: write the constant `0x47504750` to a fixed logical global-memory
offset above the shared-memory aperture, then return.

The kernel and host runner shall use these properties:

- one warp; every lane may issue the same idempotent sentinel store;
- RV32IMF instructions already covered by HLS tests;
- one aligned 32-bit global store;
- a logical result offset greater than `SHARED_MEM_SIZE_BYTES` so the request
  reaches `memory_pipeline` instead of per-CU shared memory;
- separate OCM-linked and DDR-linked ELF images only when their physical load
  addresses differ;
- no Rodinia, PTX runtime, floating-point, barriers, caches, interrupts, or
  multi-CU dependency in the first gate.

The result address must be derived as `global_mem_base + logical_result_offset`.
Both values and the final physical address are printed before launch.

## 5. Memory Strategy

### 5.1 Fast probe: OCM

OCM can avoid changing Linux DDR reservations and is suitable for a one-word
sentinel if the generated Vivado address map proves that the PL master can
reach the selected OCM segment. The runner may use it only after:

- the address segment is present in the HWH/address report;
- ARM write/read succeeds through the intended mapping;
- the kernel ELF and register seed area do not overlap the result word;
- the PL-written signature passes readback.

An accessible ARM OCM mapping alone does not prove PL reachability. If the
Vivado map excludes OCM or the first PL transaction faults, stop this probe and
move to reserved DDR.

### 5.2 Canonical path: reserved DDR

Reserved DDR is required immediately after the sentinel proof and becomes the
canonical path for vector add and later workloads. Prefer a reserved-memory
device-tree node exposed through UIO/CMA/dma-buf. The existing
`memmap=64M$0x60000000` plus `/dev/mem` setup is acceptable only as a bounded
bring-up mechanism.

Before launch, record `/proc/cmdline` or the device-tree reservation and prove
that Linux cannot allocate the region. Define cache ownership and perform the
required flush/invalidate operations around host-to-PL and PL-to-host handoff.

## 6. Execution Sequence

### Slice A: Make evidence fail closed

- KFP-001 **Done on this branch**: change `fpga_smoke_test` so inaccessible memory or an unimplemented
  launch returns failure, never PASS.
- KFP-002 **Done on this branch**: change `deploy_kria.sh` to require an explicit success marker in
  addition to exit code and to record hashes and board metadata.
- KFP-003 Preserve the old AXI-only run as control-plane evidence, not as the
  current execution report.

### Slice B: Build a deterministic first-kernel runner

- KFP-004 **Done on this branch**: add `kria_first_kernel_test`, based on the useful ELF/control code in
  `fpga_rodinia_bench` but with no benchmark naming or claims.
- KFP-005 **Done on this branch**: package generated scheduler and memory-pipeline register metadata;
  fail build/deployment when expected banks, offsets, or widths drift.
- KFP-006 **Done on this branch**: map scheduler control, scheduler pointer control, memory-pipeline
  control, and the selected memory region.
- KFP-007 **Implemented; hardware validation pending**: start `memory_pipeline`, launch the scheduler, validate status
  transitions, and check the sentinel result.

### Slice C: Run the physical proof

- KFP-008 **Done on this branch**: build the sentinel OCM and DDR ELFs and validate their headers,
  disassembly, load ranges, and expected store instruction.
- KFP-009 **Blocked for the current bitstream**: the block design marks
  `HPC0_LPS_OCM` excluded for all four PL masters and the final HWH omits it.
- KFP-010 Configure reserved DDR and rerun the same sentinel on the canonical
  path.
- KFP-011 Capture raw logs, hashes, metadata, poison/expected/actual values,
  timing, status transitions, and the exact command.

The canonical command is:

```bash
source scripts/setup-env.sh
scripts/run_kria_first_kernel.sh \
  --bitstream build/vivado_kv260/riscv_gpgpu_kv260.runs/impl_1/gpgpu_system_wrapper.bit \
  --hwh build/vivado_kv260/riscv_gpgpu_kv260.gen/sources_1/bd/gpgpu_system/hw_handoff/gpgpu_system.hwh \
  --memory ddr \
  --host ubuntu@kria
```

Reserve the DDR aperture before this run. The entrypoint validates the HWH,
packages a Vivado `.bit` into `.bit.bin` with `bootgen`, and records the HWH
hash. `--memory ocm` is rejected until a rebuilt HWH includes OCM on all four
PL masters. The command must fail when metadata, packaging, SSH, mapping,
launch, result verification, or the success marker is missing.

### Slice D: Move from proof to useful kernel

- KFP-012 Run vector add with initialized input/output buffers and exact
  element-by-element readback.
- KFP-013 Run SAXPY with a declared floating-point tolerance.
- KFP-014 Only after KFP-012/KFP-013 pass, resume T110 SIMT tests and T111
  Rodinia evidence.

## 7. Required Runner Behavior

The first-kernel runner must:

- use generated register definitions or validate hard-coded offsets against
  packaged `component.xml`/HWH metadata;
- print all mapped physical ranges and reject overlaps;
- verify ELF machine, class, endianness, load bounds, instruction count, and
  unsupported compressed instructions;
- read back loaded program bytes before launch;
- use volatile/device-safe accesses plus explicit memory barriers and cache
  maintenance appropriate to the selected region;
- initialize result memory with a poison value and verify the value changed;
- use monotonic timeout handling and print `busy`, `done`, and `fault` samples;
- clean up mappings on every error path;
- never emit PASS for a skip, unavailable region, or status-only completion.

## 8. Evidence Package

The physical report shall contain:

- repository commit and dirty-tree state;
- board model/identifier, OS/kernel, and `/proc/cmdline`;
- Vivado/Vitis versions;
- bitstream, ELF, runner, HWH/component metadata names and SHA-256 hashes;
- physical address map and selected memory strategy;
- ELF entry/load ranges and generated register offsets;
- poison, expected, and actual result values;
- launch/status timeline and elapsed time;
- exact deployment command and unedited raw output;
- explicit result: `VALIDATED ON HARDWARE` or `FAILED`, never an ambiguous PASS.

Store the curated report in `docs/verification/kria_results.md`; keep large raw
logs and generated FPGA artifacts outside Git and reference them by hash/path.

## 9. Deferred Until After First PASS

The following do not block the vertical slice:

- final abstract CSR wrapper and full T098 generation pipeline;
- production UIO/CMA/dma-buf driver and interrupt handling;
- complete host API/runtime backend separation;
- PTX-at-launch and full kernel ABI compliance;
- multi-CU scaling, performance tuning, caches, and HBM portability;
- divergence, barriers, Rodinia, statistical runs, and publication claims.

They are deferred, not waived. After KFP-012 demonstrates vector add through
canonical DDR, return to T096-T108 and replace bring-up shortcuts with the
portable, generated, coherent interfaces required by the specification.

## 10. Go/No-Go Rules

- **Go to implementation**: generated control metadata is available and at
  least one host/PL memory region has a credible mapping path.
- **Stop and repair shell**: memory-pipeline control is absent, its AXI master
  cannot reach the selected segment, or status has no reliable fault path.
- **Stop and repair kernel/HLS**: ELF readback passes but the store faults,
  times out, or leaves the poison unchanged.
- **Go to vector add**: sentinel passes twice from a clean deployment with the
  same hashes and no manual register pokes outside the runner.
- **Go to compliance work**: vector add and SAXPY pass through reserved DDR with
  reproducible deployment evidence.