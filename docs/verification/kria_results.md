# Kria Deployment Report

- Date: 2026-09-05 07:12 UTC
- Source commit: `ffdc7d4c97f2e493ff4c23d9b888a3b45de6a72f`
- Source tree: DIRTY
- Board: ubuntu@kria
- Model: ZynqMP KV260 revB
- Kernel: Linux kria 5.15.0-1027-xilinx-zynqmp #31-Ubuntu SMP Wed Feb 21 04:33:09 UTC 2024 aarch64 aarch64 aarch64 GNU/Linux
- Command line:  root=LABEL=writable rootwait earlycon console=ttyPS1,115200 console=tty1 clk_ignore_unused uio_pdrv_genirq.of_id=generic-uio xilinx_tsn_ep.st_pcp=4 cma=1000M 
- Bitstream: gpgpu_cuprogram_fix.bit.bin
- Bitstream SHA-256: `26aee178cad549352cddc43f788932363145e6bc86adb91b83a6d4c482c19178`
- Kernel ELF: kria_sentinel_nomem_ddr.elf
- Kernel SHA-256: `2062a2d640abc0eb96a396b38c7a81c9f4c69a6c8c6c578d784ed82b82cdb8d3`
- Test: kria_first_kernel_test
- Test SHA-256: `e1821840edb4c1cf8a1a23e9b68451aa3ecc41d7a61a34d04f0ca533ac9ba3c4`
- Hardware metadata: gpgpu_system.hwh
- Hardware metadata SHA-256: `c9e87c62522a07b87c9ad2366373281c19d1b0dd717850a523ffb9409e6f594e`
- Required marker: `PASS: KRIA_FIRST_KERNEL`
- Result: **FAILED**

## Test output

```
[sudo] password for ubuntu: Time taken to load BIN is 134.000000 Milli Seconds
BIN FILE loaded through FPGA manager successfully
KRIA_FIRST_KERNEL region=ddr physical_base=0x60000000 result_offset=0x00010000
mapped scheduler-control [0xa0000000,0xa0001000)
mapped scheduler-pointers [0xa0010000,0xa0011000)
mapped memory-control [0xa0020000,0xa0021000)
mapped scheduler-status [0xa0030000,0xa0031000)
mapped ddr [0x60000000,0x64000000)
PT_LOAD vaddr=0x60000000 filesz=8 memsz=8 readback=OK
program=0x60000000 words=2 regs=0x60100000 result=0x60010000 poison=0xdeadbeef
control after launch scheduler_ap=0x00000001 start_r=0x00000001 memory_ap=0x00000081 status=0x000003d1
status ready_seen=0 busy_seen=1 done=0 fault=0 elapsed_ms=10001 actual=0xdeadbeef final_status=0x000003d1
FAIL: KRIA_FIRST_KERNEL expected=0x47504750 actual=0xdeadbeef done=0 fault=0
```
