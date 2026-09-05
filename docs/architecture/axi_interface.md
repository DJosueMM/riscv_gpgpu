# GPGPU AXI Interface Definition (Kria KV260/KR260)

**Task**: T050, superseded for reconciliation by T096b-T098
**Status**: Provisional logical contract; not proven equivalent to generated HLS/Vivado interfaces
**Code mirror**: [driver/src/fpga_regs.h](../../driver/src/fpga_regs.h)

This document defines the hardware/software contract between the ARM PS
(Cortex-A53, Linux userspace driver) and the PL GPGPU on the AMD Kria
KV260/KR260. Any change to this contract must be applied simultaneously to
`driver/src/fpga_regs.h` and to the HLS/RTL top-level ports.

> **Conformance warning:** the `ID/CTRL/STATUS` map below is the intended
> logical contract, not the verified physical map of the current bitstream.
> The current Vivado design exposes multiple HLS-generated AXI4-Lite banks and
> four full AXI masters through one SmartConnect/HPC0 path. T098 must either
> generate all consumers from one description or implement a real RTL wrapper
> before this document can return to `Defined` status.

## Interface selection rule

Per [ADR-0006](decisions/0006-control-data-plane-separation.md) and the
[performance strategy](performance_strategy.md):

- AXI4-Lite is the baseline for low-rate configuration, doorbell, status,
   faults, interrupt control, and bounded counter snapshots.
- AXI4-Lite is not used for program images, contexts, kernel buffers, results,
   or high-volume traces.
- Full AXI4 and managed buffers/DMA carry bulk traffic.
- A descriptor queue plus AXI4-Lite doorbell replaces or complements direct
   CSR launch only if T096b measurements show control overhead above 1% for a
   target workload or a launch-rate requirement is missed.

The control bus is not the current steady-state throughput hypothesis. The
one-outstanding-per-CU, N:1 memory path and single Kria HPC port must be
measured under T096c-T096g and T105 before changing widths or adding CUs.

---

## 1. Address map overview

| Region                     | Physical base | Size    | Bus            | Direction |
|----------------------------|---------------|---------|----------------|-----------|
| Control/status registers   | `0xA000_0000` | 4 KiB   | AXI4-Lite slave (PS GP0 → PL) | PS writes/reads |
| FPGA global memory aperture| `0x6000_0000` | 64 MiB  | AXI4 (PL masters → DDR)       | PL reads/writes, PS via `/dev/mem` or DMA proxy |

The global memory aperture holds both **instruction memory** (kernel ELF
`PT_LOAD` segments, loaded at their link-time virtual addresses) and **data
memory** (device buffers allocated by the driver).

## 2. AXI4-Lite register block

* Slave port: `s_axi_ctrl`, 32-bit data, 12-bit address (4 KiB window).
* All registers are 32-bit, little-endian, word-aligned.
* Reserved offsets (`0x1C`–`0xFFC`) read as `0` and ignore writes.

| Offset | Name         | Access | Reset value  | Description |
|--------|--------------|--------|--------------|-------------|
| `0x00` | `ID`         | RO     | `0x4750_5501`| Device identification/version (`"GPU"` + v1). The driver validates this before any other access. |
| `0x04` | `CTRL`       | RW     | `0x0`        | Bit 0 `START` (self-clearing): begin kernel execution. Bit 1 `RESET`: synchronous reset of the compute pipeline, returns `STATUS` to `IDLE`. Bit 2 `IRQ_CLEAR`: acknowledge the done interrupt. |
| `0x08` | `STATUS`     | RO     | `0x0` (IDLE) | `0`=IDLE, `1`=RUNNING, `2`=DONE, `3`=ERROR. |
| `0x0C` | `PC_INIT`    | RW     | `0x0`        | RISC-V entry point PC written by the ELF loader (`e_entry`). Latched when `CTRL.START` is asserted. |
| `0x10` | `GRID_X`     | RW     | `0x1`        | Launch grid dimension X. |
| `0x14` | `GRID_Y`     | RW     | `0x1`        | Launch grid dimension Y. |
| `0x18` | `IRQ_ENABLE` | RW     | `0x0`        | Bit 0: enable level interrupt on `STATUS == DONE`. |

### Execution handshake

1. Driver writes `PC_INIT`, `GRID_X`, `GRID_Y` (T052/T053).
2. Driver writes `CTRL.START = 1`; hardware clears the bit and moves `STATUS`
   to `RUNNING` (must occur within 10 ms — verified by T053).
3. On completion, hardware sets `STATUS = DONE` and, if `IRQ_ENABLE.DONE` is
   set, raises the `irq_done` line (PL→PS IRQ0, mapped to a UIO device).
4. Driver reads results, then writes `CTRL.RESET = 1` to return to `IDLE`.

Any AXI decode error, illegal instruction, or memory fault moves `STATUS` to
`ERROR`; only `CTRL.RESET` leaves that state.

## 3. Logical AXI4 Data Channels

The two channels below describe logical program and data responsibilities.
They are not current physical HLS port names. The present block design connects
`gpgpu_scheduler` masters `m_axi_gmem0`, `m_axi_gmem1`, and `m_axi_gmem2` plus
`memory_pipeline/m_axi_gmem` through one SmartConnect to Kria HPC0. T098 and
T096f decide the final mapping.

| Port         | Type        | Width | Purpose |
|--------------|-------------|-------|---------|
| `m_axi_imem` | AXI4 master | 64-bit data, 32-bit address | Instruction memory load: fetches kernel code from the global memory aperture where the ELF loader placed the `PT_LOAD` segments (H2D, T052). |
| `m_axi_dmem` | AXI4 master | 64-bit data, 32-bit address | Data memory: kernel loads/stores against device buffers (H2D before launch, D2H after `STATUS == DONE`). |

Host-side transfers into the aperture use `mmap()` of `/dev/mem` at
`0x6000_0000` (or a kernel DMA-proxy character device when cache coherency
management is required). Buffers are 16-byte aligned; the driver implements a
bump allocator over the aperture (see `driver/src/fpga_driver.cpp`).

## 4. Interrupt

| Signal     | Type            | Mapping |
|------------|-----------------|---------|
| `irq_done` | Level, active-high | PL→PS `pl_ps_irq0`, exposed to userspace as `/dev/uioN` ("gpgpu-ctrl"). |

`gpgpuSynchronize()` (T054) either blocks on a `read()` of the UIO device or
polls `STATUS == DONE` with a timeout; both paths are implemented.

## 5. Device tree fragment (reference)

```dts
gpgpu@a0000000 {
    compatible = "generic-uio";
    reg = <0x0 0xa0000000 0x0 0x1000>;
    interrupt-parent = <&gic>;
    interrupts = <0 89 4>;   /* pl_ps_irq0 */
};
```

## 6. Constraints

Clock and reset constraints for the PL implementation live in
[fpga/constraints/kv260_gpgpu.xdc](../../fpga/constraints/kv260_gpgpu.xdc).

---

## 7. Control/Data Plane Split (V2)

This section defines the target interface requested for higher throughput:

- One independent AXI4-Lite slave dedicated to control and CSRs.
- Separate AXI4 master path(s) dedicated to high-bandwidth payload data.

The intent is to keep launch/status deterministic on a low-latency control bus
while isolating payload traffic from control arbitration.

### 7.1 Bus topology

| Plane | Port | Protocol | Typical width | Purpose |
|------|------|----------|---------------|---------|
| Control | `s_axi_ctrl` | AXI4-Lite slave | 32-bit | Enable, launch config, doorbell, status, IRQ control, perf snapshots |
| Data | `m_axi_data` (or `m_axi_data{0..N}`) | AXI4 master | 128/256-bit | Kernel/global-memory payload reads and writes |
| Optional descriptor fetch | `m_axi_desc` | AXI4 master | 64/128-bit | Descriptor ring reads when queue mode is enabled |

Recommended Kria mapping:

- `s_axi_ctrl` via PS GP to a 4 KiB aperture.
- `m_axi_data*` via HPC/HP path(s) to DDR through SmartConnect.
- If only one HPC is available, prioritize bandwidth and burst tuning on
   `m_axi_data*`; keep control isolated regardless.

### 7.2 AXI4-Lite CSR map (V2)

Mirror in code: [driver/src/fpga_regs.h](../../driver/src/fpga_regs.h) under
`namespace riscv_gpgpu::fpga::v2`.

| Offset | Name | Access | Description |
|-------:|------|--------|-------------|
| `0x000` | `ID` | RO | Device ID/version (`0x47505502` for V2). |
| `0x004` | `CTRL` | RW | `ENABLE`, `START` (self-clear), `RESET`, `IRQ_CLEAR`. |
| `0x008` | `STATUS` | RO | `BUSY`, `DONE`, `FAULT`, `READY`, `ENABLED`, `CONFIGURED`, `PLL_LOCKED`. |
| `0x00C` | `IRQ_ENABLE` | RW | Interrupt mask bits (`DONE`, `FAULT`). |
| `0x010` | `IRQ_STATUS` | RW1C | Latched IRQ cause bits (`DONE`, `FAULT`). |
| `0x014` | `WARP_ID_OFFSET` | RW | Global warp start index for this device. |
| `0x018` | `TOTAL_WARPS` | RW | Number of warps assigned to this device. |
| `0x01C` | `PROGRAM_LEN` | RW | Program length in instruction words. |
| `0x020` | `PC_INIT_LO` | RW | Entry PC low 32 bits (or descriptor ptr low). |
| `0x024` | `PC_INIT_HI` | RW | Entry PC high 32 bits (or descriptor ptr high). |
| `0x028` | `DESC_BASE_LO` | RW | Descriptor ring base low 32 bits. |
| `0x02C` | `DESC_BASE_HI` | RW | Descriptor ring base high 32 bits. |
| `0x030` | `DESC_STRIDE_BYTES` | RW | Bytes per descriptor entry. |
| `0x034` | `DESC_COUNT` | RW | Number of valid descriptors in ring window. |
| `0x038` | `DOORBELL` | WO | Submit descriptors / kick processing. |
| `0x03C` | `CAPABILITIES` | RO | Feature bits (`DESC_QUEUE`, `SPLIT_AXI_DATA`, `64B_ADDR`). |
| `0x040..0x058` | Perf counters | RO | 64-bit instruction/L1 counters split LO/HI, divergence counter. |

Reserved space through `0xFFF` reads as `0` and ignores writes.

### 7.3 Host launch handshake (V2)

Required sequence:

1. Read `STATUS.READY==1` and `STATUS.PLL_LOCKED==1`.
2. Program `WARP_ID_OFFSET`, `TOTAL_WARPS`, `PROGRAM_LEN`, `PC_INIT_*` (or
    descriptor registers for queue mode).
3. Set `CTRL.ENABLE=1`.
4. Re-check `STATUS.READY==1` and `STATUS.ENABLED==1`.
5. Pulse `CTRL.START=1` (or write `DOORBELL` in descriptor mode).
6. Wait for `STATUS.DONE` or IRQ; on fault, inspect diagnostics and clear with
    `CTRL.RESET`/`CTRL.IRQ_CLEAR` as required.

This matches the software-side staged flow already modeled in `SystemTopHLS`.

### 7.4 Compatibility policy

- V1 map remains valid for current bitstreams.
- V2 is additive and should be selected by checking `ID` and/or
   `CAPABILITIES`.
- Driver may support both maps concurrently during migration.
