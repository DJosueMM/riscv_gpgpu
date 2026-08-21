# Architecture Docs

This directory contains architecture contracts and design references.

Scope:
- interface definitions between software, model, and FPGA control plane
- ISA and execution-semantics reference docs

## Files

| File | Purpose |
|---|---|
| `ARCHITECTURE.md` | High-level architecture narrative |
| `platform_strategy.md` | Portable core, Kria/U55C roles, gates, and T093-T128 roadmap |
| `performance_strategy.md` | Performance questions, analytical ceilings, experiments, and stop conditions |
| `decisions/` | Architecture decision records and proposed decisions |
| `interfaces.md` | Cross-component interface contracts |
| `isa.md` | ISA and execution model notes |
| `axi_interface.md` | AXI4-Lite register map and AXI4 DMA contract for Kria deployment |

## Ownership Rules

- Register/DMA interface changes must be mirrored in code constants
	(`driver/src/fpga_regs.h`).
- Cross-platform decisions belong in ADRs; proposed ADRs are not implementation claims.
- The platform strategy owns target roles and evidence gates, not register-level details.
- Detailed implementation behavior belongs in component source trees, not here.
