# Kria Split-Control V2 Synthesis Guide

This guide captures the practical migration step toward a control/data plane split on Kria KV260.

## Goal

- Keep control in a dedicated AXI4-Lite plane (CSRs, status, IRQ, launch gate).
- Keep payload traffic in a separate AXI4 high-bandwidth plane to DDR.

## What is implemented now

- V2 logical CSR map and handshake in software/driver.
- Kria synthesis helper script that repacks existing AXI-Lite slaves into a compact control region.

Script:

- fpga/scripts/synth_kv260_split_ctrl_v2.tcl

## Run on a machine with Vivado

1. Build full project baseline if needed:

```bash
vivado -mode batch -source fpga/scripts/build_all.tcl
```

2. Run split-control V2 synthesis variant:

```bash
vivado -mode batch -source fpga/scripts/synth_kv260_split_ctrl_v2.tcl
```

3. Inspect reports:

- build/vivado_kv260/synthesis_utilization_split_ctrl_v2.rpt
- build/vivado_kv260/synthesis_timing_split_ctrl_v2.rpt

## Control map used by the migration script

- 0xA0000000 : gpgpu_scheduler / s_axi_control
- 0xA0001000 : gpgpu_scheduler / s_axi_control_r
- 0xA0002000 : memory_pipeline / s_axi_control
- 0xA0003000 : scheduler status / AXI GPIO

## Important limitation

This migration step does not yet create a single physical AXI-Lite CSR bank.
It keeps existing HLS AXI-Lite slaves and packs them into one compact control region.

To match the full V2 target exactly, add an RTL CSR wrapper that exposes one AXI-Lite slave and drives/observes the HLS internals.

## Next hardware step (recommended)

- Implement `gpgpu_ctrl_csr_v2` wrapper (single AXI-Lite slave):
  - Owns ID/CTRL/STATUS/IRQ/doorbell registers.
  - Drives launch/control signals toward scheduler/memory IP.
  - Latches status/perf counters from status streams or GPIO bridge.
- Update block design to expose only that wrapper to PS GP0.
- Keep AXI4 data masters on HPC path unchanged.
