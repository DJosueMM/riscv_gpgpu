extern "C" void kria_sentinel() {
    constexpr unsigned long kResultOffset = 0x00010000u;
    constexpr unsigned int kExpectedSignature = 0x47504750u;

    auto* result = reinterpret_cast<volatile unsigned int*>(kResultOffset);
    *result = kExpectedSignature;

    // Custom-1, funct3=3: HALT in the HLS RV32 decoder.
    asm volatile(".word 0x0000302b" ::: "memory");
}