# RISC-V GPGPU

Open research platform for a RISC-V GPGPU flow spanning:
- SystemC functional simulation
- host software stack (launch/memory/runtime)
- HLS/RTL path for Kria FPGA deployment

This project is provided under the MIT License. See the [LICENSE](LICENSE) file for details.

This README is intentionally brief and only covers repository entry points.
Component details live in each component README.

## Scope Map

| Path | Owner scope |
|---|---|
| `models/systemc/` | Functional model and SystemC integration details |
| `software/` | Host-facing software APIs and compiler/runtime interfaces |
| `benchmarks/` | Benchmark binaries, workloads, and analysis artifacts |
| `hls/` | HLS implementation plan and constraints |
| `docs/architecture/` | Hardware/software architecture contracts |
| `docs/traceability/` | Requirement-to-evidence mapping |
| `docs/reproducibility/` | Reproducible environment and run procedures |
| `specs/001-open-riscv-gpgpu/` | Spec, plan, and task checklist (source of truth) |

## FPGA Architecture Roadmap

The approved path separates a portable accelerator core from Kria and Alveo
platform shells. Kria is the first physical bring-up platform; Alveo U55C is
the final scale-out target after a timeboxed XRT-versus-RTL shell comparison.

- [Platform strategy and gates](docs/architecture/platform_strategy.md)
- [Performance and interface decision strategy](docs/architecture/performance_strategy.md)
- [Architecture decisions](docs/architecture/decisions/README.md)
- [Execution plan](specs/001-open-riscv-gpgpu/plan.md#14-current-architecture-roadmap)
- [Task ledger T093-T128](specs/001-open-riscv-gpgpu/tasks.md#phase-8-architecture-rebaseline-and-fpga-roadmap)

## Quick Start

```bash
cmake -S . -B build-all -DBUILD_TESTS=ON -DBUILD_SYSTEMC_MODELS=ON
cmake --build build-all -j$(nproc)
ctest --test-dir build-all --output-on-failure
```

Optional integration path:

```bash
cmake -S . -B build-all -DBUILD_SYSTEMC_INTEGRATION=ON -DBUILD_TESTS=ON
cmake --build build-all -j$(nproc)
ctest --test-dir build-all --output-on-failure
```

## Common Commands

```bash
# Full verification harness
bash scripts/verify.sh

# Rodinia matrix benchmark
bash scripts/benchmark/run_rodinia_real_matrix.sh

# Kria deployment helper (Phase 7)
bash scripts/deploy_kria.sh --help
```

## Current Stability Snapshot

- Core build and test flow is green in local CI-style runs.
- HLS IP export and the full KV260 Vivado bitstream flow pass locally.
- Kria AXI-Lite bring-up succeeded, but DDR and kernel execution were skipped.
- End-to-end physical kernel validation remains open and must not be reported as PASS.

## Project Status

This repository is intended as a collaborative open-source research platform. It is preconfigured for shared development, issue tracking, and pull requests.

## Where To Update Status

- Task progress: `specs/001-open-riscv-gpgpu/tasks.md`
- Verification evidence: `docs/verification/`
- Traceability matrix: `docs/traceability/traceability_matrix.md`
