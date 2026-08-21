# Traceability Matrix

Validated requirements-to-evidence chain for major artifacts.

## Requirements to Implementation and Evidence

| Req ID | Requirement | Major implementation artifacts | Verification artifacts | Status |
|---|---|---|---|---|
| REQ-EXE-001 | RV32 kernel execution through SystemC integration | `models/systemc/src/compute_unit/`, `models/systemc/integration/kernel_bridge.cpp`, `models/systemc/integration/elf_loader.cpp` | `tests/systemc/test_systemc_integration.cpp`, CTest systemc suites | Implemented |
| REQ-SIMT-001 | SIMT divergence and reconvergence behavior | `models/systemc/src/simt_controller/`, `models/systemc/integration/kernel_bridge.cpp` | `tests/systemc/test_simt_controller.cpp`, integration divergence checks | Implemented |
| REQ-SCHED-001 | Multi-CU warp scheduling behavior | `models/systemc/src/scheduler/warp_scheduler.cpp`, `models/systemc/src/top/top.cpp` | `tests/systemc/test_scheduler.cpp` | Implemented |
| REQ-MEM-001 | Shared/global memory behavior and barrier synchronization | `models/systemc/src/memory/memory_hierarchy.cpp`, `models/systemc/src/compute_unit/compute_unit.cpp` | `tests/systemc/test_shared_memory.cpp` | Implemented |
| REQ-COMP-001 | PTX to RISC-V compilation path | `driver/src/ptx_transpiler/` | `tests/compiler/ptx/test_ptx_parser.cpp`, `tests/compiler/ptx/test_rv_emitter.cpp`, `tests/compiler/ptx/test_ptx_transpiler.cpp` | Implemented |
| REQ-BENCH-001 | End-to-end benchmark execution and metric capture | `benchmarks/rodinia_real_benchmark.cpp`, `scripts/benchmark/run_rodinia_real_matrix.sh`, `scripts/benchmark/analyze_results.py` | `results/benchmarks/rodinia_real_matrix/summary.tsv`, `results/benchmarks/rodinia_real_matrix/summary.json`, `docs/verification/benchmark_results.md` | Implemented |
| REQ-FPGA-001 | ARM host to FPGA control plane for Kria deployment | `driver/src/fpga_driver.cpp`, `driver/src/fpga_elf_loader.cpp`, `driver/src/fpga_regs.h`, `software/host_api/host_api.cpp`, `scripts/deploy_kria.sh` | `tests/fpga/test_fpga_driver.cpp`, HLS/Vivado reports, `docs/verification/kria_results.md` | Implemented and synthesized; physical kernel execution not validated |
| REQ-015 | Portable accelerator core with isolated Kria and U55C shells/backends | `docs/architecture/platform_strategy.md`, ADR-0001 | T096, T106, T113-T116 conformance evidence | Planned |
| REQ-016 | Canonical machine-checkable ISA/ABI, CSR, memory, configuration, and trace contracts | ADR-0002, ADR-0003, `config/arch_config.yaml` | T097-T101 generated-artifact and drift tests | Planned; ADR-0002 accepted |
| REQ-017 | Physical kernel load, launch, completion, readback, and result verification | Kria and U55C platform paths | T109-T110 and T116 physical reports | Planned; current Kria PASS invalid for this gate |
| REQ-018 | Kria bring-up and measured U55C scale-out selection | `docs/architecture/platform_strategy.md`, ADR-0004, ADR-0005 | T107-T117 decision/build/hardware evidence | Planned |
| REQ-019 | Comparable execution signature across SystemC, HLS/RTL, and hardware | Observability contract to be defined by T101 | T101-T103 and T110 parity reports | Planned |
| REQ-020 | Workload-driven separation and selection of control, data, request, and trace planes | `docs/architecture/performance_strategy.md`, ADR-0006 | T096a-T096g, T100-T105, and platform bandwidth/scaling reports | Planned; plane separation accepted |
| NFR-008 | Explicit evidence maturity with no false PASS on skipped prerequisites | Task ledger and traceability process | T093 audit, T108 deployment failure paths, T128 release gate | In progress |
| NFR-009 | Balanced statistical evaluation of correctness, performance, scalability, resources, and energy | Benchmark harness to be defined by T118-T119 | T119-T123 raw data and reports | Planned |

## Evidence States

| State | Meaning |
|---|---|
| Planned | Approved work without an implementation claim |
| Implemented | Source exists and passes its scoped local checks |
| Simulated | The required functional or cosimulation gate passed |
| Synthesized | HLS or FPGA implementation completed with reports |
| Validated on hardware | A physical kernel completed and its result was checked |
| Invalid evidence | The artifact does not satisfy the gate it was cited for |
| Superseded | A newer task or decision replaced the artifact |

## Evidence Chain Status

| Chain stage | Artifact location | Validation state |
|---|---|---|
| Requirement and task intent | `specs/001-open-riscv-gpgpu/tasks.md` | Updated through Phase 7 + Phase 6 documentation tasks |
| Architecture and interface contracts | `docs/architecture/` | Updated (includes AXI contract) |
| Implementation | `models/`, `driver/`, `runtime/`, `software/`, `benchmarks/` | Present and buildable |
| Automated verification | `tests/`, CTest suites | Passing in local run baseline |
| Benchmark evidence | `results/benchmarks/`, `docs/verification/benchmark_results.md` | Captured |
| HLS/Vivado implementation | Generated HLS and KV260 reports | Passing locally; curated report package pending |
| Kria control-plane bring-up | `docs/verification/kria_results.md` | AXI-Lite reached; DDR and kernel execution skipped |
| FPGA hardware execution | T109-T111 and T116 | Pending physical kernel run |

## Gaps

- The documented abstract CSR map and generated HLS control banks have not been proven equivalent; T098 owns reconciliation.
- The current one-outstanding-per-CU, N:1 runtime memory path has not demonstrated useful bandwidth scaling; T096c-T096g and T105 own the decision evidence.
- Kria DDR mapping and coherent transport are unresolved; T107 owns the transport contract.
- Hardware execution evidence is pending. The current Kria report is invalid for T087-T089/T092 and REQ-017 because it skipped the kernel.
- U55C shell selection and all U55C physical evidence remain planned pending T112-T117 and hardware access.
