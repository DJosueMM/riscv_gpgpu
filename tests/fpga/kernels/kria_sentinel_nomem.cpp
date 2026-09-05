// Diagnostic-only variant of kria_sentinel: HALTs immediately with no
// memory access, to isolate whether a hang is in the mem_req_out/
// mem_resp_in round-trip or earlier in dispatch/decode.
extern "C" void kria_sentinel_nomem() {
    asm volatile(".word 0x0000302b" ::: "memory");  // custom-1 funct3=3: HALT
}
