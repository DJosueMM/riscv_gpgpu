# Kria Deployment Report

- Date: 2026-09-05 02:51 UTC
- Source commit: `9fcab4c340ad59c7b2a0d1d7062dfa9353db24ad`
- Source tree: DIRTY
- Board: kria
- Model: ZynqMP KV260 revB
- Kernel: Linux kria 5.15.0-1027-xilinx-zynqmp #31-Ubuntu SMP Wed Feb 21 04:33:09 UTC 2024 aarch64 aarch64 aarch64 GNU/Linux
- Command line:  root=LABEL=writable rootwait earlycon console=ttyPS1,115200 console=tty1 clk_ignore_unused uio_pdrv_genirq.of_id=generic-uio xilinx_tsn_ep.st_pcp=4 cma=1000M 
- Bitstream: gpgpu_system_wrapper_swap_test.bit.bin
- Bitstream SHA-256: `1ec385ba9f917ec258fda73b3f9c4e0a73b1aa14ce18d14b8635e65e925d7f3f`
- Kernel ELF: kria_sentinel_ddr.elf
- Kernel SHA-256: `434cd6a87598b0b34cda9f058002aac7b7c310d65dbd757f0eb423224a86a387`
- Test: kria_first_kernel_test
- Test SHA-256: `0488268b8c73ad851b49eaf2b1037829abb78c8081e62096278850a36a8f10b8`
- Hardware metadata: gpgpu_system.hwh
- Hardware metadata SHA-256: `1db66482f9ad361029052dcd74477a719d23aaff7d28e3df0657c676adf899c9`
- Required marker: `PASS: KRIA_FIRST_KERNEL`
- Result: **FAILED**

## Test output

```
[sudo] password for ubuntu: Time taken to load BIN is 135.000000 Milli Seconds
BIN FILE loaded through FPGA manager successfully
KRIA_FIRST_KERNEL region=ddr physical_base=0x60000000 result_offset=0x00010000
mapped scheduler-control [0xa0000000,0xa0001000)
mapped scheduler-pointers [0xa0010000,0xa0011000)
mapped memory-control [0xa0020000,0xa0021000)
mapped scheduler-status [0xa0030000,0xa0031000)
mapped ddr [0x60000000,0x64000000)
PT_LOAD vaddr=0x60000000 filesz=24 memsz=24 readback=OK
program=0x60000000 words=6 regs=0x60100000 result=0x60010000 poison=0xdeadbeef
control before launch scheduler_ap=0x00000004 start_r=0x00000000 memory_ap=0x00000081 status=0x00000000
ERROR: scheduler start_r did not latch after 5 attempts (ap_ctrl=0x00000004 start_r=0x00000000)
```
