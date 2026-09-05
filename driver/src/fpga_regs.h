// fpga_regs.h - GPGPU AXI4-Lite register map (single source of truth for code)
//
// Mirrors docs/architecture/axi_interface.md. Any change here must be
// reflected in that document and in the HLS/RTL top-level ports.

#ifndef RISCV_GPGPU_FPGA_REGS_H
#define RISCV_GPGPU_FPGA_REGS_H

#include <cstdint>

namespace riscv_gpgpu {
namespace fpga {

// ── AXI4-Lite control/status register block (PS → PL) ───────────────────────
// Default physical base on Kria KV260/KR260 (PL AXI GP0 window).
constexpr uint64_t kRegBlockPhysBase = 0xA0000000ULL;
constexpr uint32_t kRegBlockSize     = 0x1000;  // 4 KiB window

// Register byte offsets inside the AXI4-Lite window (32-bit registers).
constexpr uint32_t REG_ID         = 0x00;  // RO  device id/version, expect kDeviceId
constexpr uint32_t REG_CTRL       = 0x04;  // RW  bit0=START (self-clearing), bit1=RESET, bit2=IRQ_CLEAR
constexpr uint32_t REG_STATUS     = 0x08;  // RO  execution state, see Status enum
constexpr uint32_t REG_PC_INIT    = 0x0C;  // RW  kernel entry point (RISC-V PC)
constexpr uint32_t REG_GRID_X     = 0x10;  // RW  launch grid dimension X
constexpr uint32_t REG_GRID_Y     = 0x14;  // RW  launch grid dimension Y
constexpr uint32_t REG_IRQ_ENABLE = 0x18;  // RW  bit0=enable "done" interrupt

// REG_ID expected value: 'RGPU' spelled in hex nibbles + version 0x01.
constexpr uint32_t kDeviceId = 0x47505501;  // "GPU" + v1

// CTRL bit fields.
constexpr uint32_t CTRL_START     = 1u << 0;
constexpr uint32_t CTRL_RESET     = 1u << 1;
constexpr uint32_t CTRL_IRQ_CLEAR = 1u << 2;

// STATUS values.
enum class Status : uint32_t {
    IDLE    = 0,
    RUNNING = 1,
    DONE    = 2,
    ERROR   = 3,
};

// IRQ_ENABLE bit fields.
constexpr uint32_t IRQ_ENABLE_DONE = 1u << 0;

// ---------------------------------------------------------------------------
// V2 split-plane contract (forward compatible)
// ---------------------------------------------------------------------------
// Keeps AXI4-Lite for control/CSR while bulk traffic uses independent AXI4
// data masters. Existing software can continue using the V1 offsets above;
// new software should migrate to the map below once RTL wrapper is enabled.
namespace v2 {

// REG_ID expected value for the V2 control-plane contract.
constexpr uint32_t kDeviceId = 0x47505502;  // "GPU" + v2

// AXI4-Lite CSR map (32-bit words, 4 KiB window).
constexpr uint32_t REG_ID                = 0x000;  // RO  device id/version
constexpr uint32_t REG_CTRL              = 0x004;  // RW  enable/start/reset/irq_clear
constexpr uint32_t REG_STATUS            = 0x008;  // RO  sticky/live status bits
constexpr uint32_t REG_IRQ_ENABLE        = 0x00C;  // RW  interrupt mask
constexpr uint32_t REG_IRQ_STATUS        = 0x010;  // RW1C interrupt pending bits
constexpr uint32_t REG_WARP_ID_OFFSET    = 0x014;  // RW  global warp offset
constexpr uint32_t REG_TOTAL_WARPS       = 0x018;  // RW  warps assigned to this device
constexpr uint32_t REG_PROGRAM_LEN       = 0x01C;  // RW  instruction words for launch
constexpr uint32_t REG_PC_INIT_LO        = 0x020;  // RW  entry PC/data descriptor ptr low
constexpr uint32_t REG_PC_INIT_HI        = 0x024;  // RW  entry PC/data descriptor ptr high
constexpr uint32_t REG_DESC_BASE_LO      = 0x028;  // RW  descriptor ring base low
constexpr uint32_t REG_DESC_BASE_HI      = 0x02C;  // RW  descriptor ring base high
constexpr uint32_t REG_DESC_STRIDE_BYTES = 0x030;  // RW  bytes per descriptor
constexpr uint32_t REG_DESC_COUNT        = 0x034;  // RW  number of valid descriptors
constexpr uint32_t REG_DOORBELL          = 0x038;  // WO  submit descriptors / kick engine
constexpr uint32_t REG_CAPABILITIES      = 0x03C;  // RO  feature bitfield

// Optional perf counters (RO, snapshot semantics defined in docs).
constexpr uint32_t REG_INSTR_LO          = 0x040;
constexpr uint32_t REG_INSTR_HI          = 0x044;
constexpr uint32_t REG_L1_HITS_LO        = 0x048;
constexpr uint32_t REG_L1_HITS_HI        = 0x04C;
constexpr uint32_t REG_L1_MISSES_LO      = 0x050;
constexpr uint32_t REG_L1_MISSES_HI      = 0x054;
constexpr uint32_t REG_DIVERGENCE        = 0x058;

// CTRL bits.
constexpr uint32_t CTRL_ENABLE     = 1u << 0;  // host arms device for launch
constexpr uint32_t CTRL_START      = 1u << 1;  // self-clearing pulse
constexpr uint32_t CTRL_RESET      = 1u << 2;  // synchronous control reset
constexpr uint32_t CTRL_IRQ_CLEAR  = 1u << 3;  // clear latched IRQ cause

// STATUS bits.
constexpr uint32_t STATUS_BUSY       = 1u << 0;
constexpr uint32_t STATUS_DONE       = 1u << 1;
constexpr uint32_t STATUS_FAULT      = 1u << 2;
constexpr uint32_t STATUS_READY      = 1u << 3;
constexpr uint32_t STATUS_ENABLED    = 1u << 4;
constexpr uint32_t STATUS_CONFIGURED = 1u << 5;
constexpr uint32_t STATUS_PLL_LOCKED = 1u << 6;

// IRQ bits.
constexpr uint32_t IRQ_DONE  = 1u << 0;
constexpr uint32_t IRQ_FAULT = 1u << 1;

// CAPABILITIES bits.
constexpr uint32_t CAP_DESC_QUEUE     = 1u << 0;
constexpr uint32_t CAP_SPLIT_AXI_DATA = 1u << 1;
constexpr uint32_t CAP_64B_ADDR        = 1u << 2;

}  // namespace v2

// ── FPGA global memory window (PL DDR aperture, AXI4 masters) ───────────────
// Instruction and data memory live in a shared physical aperture that both
// AXI4 masters (instruction fetch DMA and data DMA) address.
constexpr uint64_t kGlobalMemPhysBase = 0x60000000ULL;
constexpr uint64_t kGlobalMemSize     = 64ULL * 1024 * 1024;  // 64 MiB

} // namespace fpga
} // namespace riscv_gpgpu

#endif // RISCV_GPGPU_FPGA_REGS_H
