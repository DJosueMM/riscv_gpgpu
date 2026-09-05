// Minimal physical Kria proof: load RV32 ELF, start both HLS IPs, and verify
// a sentinel value written by the PL through the runtime memory path.

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {

constexpr uint64_t kSchedulerControlBase = 0xA0000000ULL;
constexpr uint64_t kSchedulerPointerBase = 0xA0010000ULL;
constexpr uint64_t kMemoryControlBase = 0xA0020000ULL;
constexpr uint64_t kSchedulerStatusBase = 0xA0030000ULL;
constexpr size_t kControlSize = 0x1000;

constexpr uint32_t kSchedulerApCtrl = 0x00;
// NOTE (mod-16 CSR write-decode workaround - see
// /memories/session/bit3_swap_experiment.md and gpgpu_top.h): gpgpu_top.cpp
// now interleaves unused `_reserved0.._reserved3` scalars so every REAL
// register lands on a mod-16 offset (a confirmed AXI-Lite write-decode
// defect silently drops writes to any offset with AWADDR[3] or AWADDR[2]
// set). Do not "clean up" these gaps without re-verifying the bug is fixed.
constexpr uint32_t kSchedTotalWarps = 0x10;
constexpr uint32_t kSchedProgramLen = 0x20;
constexpr uint32_t kSchedStart = 0x30;
constexpr uint32_t kSchedWarpOffset = 0x40;
constexpr uint32_t kStatusData = 0x00;
constexpr uint32_t kStatusBusy = 1u << 0;
constexpr uint32_t kStatusDone = 1u << 1;
constexpr uint32_t kStatusFault = 1u << 2;
constexpr uint32_t kStatusReady = 1u << 3;
constexpr uint32_t kLaunchWriteRetries = 5;

// control_r bundle layout after the mod-16 padding fix (gpgpu_top.cpp/.h):
// each pointer's auto-generated {DATA_0,DATA_1,CTRL} block is padded out to
// 16 bytes so every pointer's DATA_0 (the meaningful low word) lands on a
// mod-16 offset. program_ptr already sat at 0x10 (mod16=0) unpadded;
// initial_regs_ptr0/1 moved from 0x1c/0x28 (broken, silently discarded) to
// 0x20/0x30 (mod16=0, confirmed latching on real hardware).
constexpr uint32_t kPtrProgram = 0x10;
constexpr uint32_t kPtrRegs0 = 0x20;
constexpr uint32_t kPtrRegs1 = 0x30;

constexpr uint32_t kMemoryApCtrl = 0x00;
constexpr uint32_t kMemoryGlobalBase = 0x10;
constexpr uint32_t kApStart = 1u << 0;
constexpr uint32_t kAutoRestart = 1u << 7;

constexpr uint32_t kResultOffset = 0x00010000u;
constexpr uint32_t kExpectedSignature = 0x47504750u;
constexpr uint32_t kPoison = 0xDEADBEEFu;
constexpr size_t kRegistersPerWarp = 32u * 32u * sizeof(uint32_t);
constexpr uint32_t kMaxProgramWords = 4096;

struct MemoryRegion {
    const char* name;
    uint64_t physical_base;
    size_t size;
    size_t register_offset;
};

constexpr MemoryRegion kOcm = {
    "ocm", 0xFFFC0000ULL, 128u * 1024u, 0x4000u
};
constexpr MemoryRegion kDdr = {
    "ddr", 0x60000000ULL, 64u * 1024u * 1024u, 0x100000u
};

#pragma pack(push, 1)
struct Elf32Header {
    uint8_t ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint32_t entry;
    uint32_t program_header_offset;
    uint32_t section_header_offset;
    uint32_t flags;
    uint16_t header_size;
    uint16_t program_header_entry_size;
    uint16_t program_header_count;
    uint16_t section_header_entry_size;
    uint16_t section_header_count;
    uint16_t section_name_index;
};

struct Elf32ProgramHeader {
    uint32_t type;
    uint32_t offset;
    uint32_t virtual_address;
    uint32_t physical_address;
    uint32_t file_size;
    uint32_t memory_size;
    uint32_t flags;
    uint32_t alignment;
};
#pragma pack(pop)

constexpr uint32_t kLoadSegment = 1;
constexpr uint32_t kExecutable = 1;
constexpr uint16_t kRiscvMachine = 243;
constexpr const char* kDdrReservationNode =
    "/sys/firmware/devicetree/base/reserved-memory/riscv_gpgpu@60000000";

struct KernelInfo {
    uint64_t program_physical = 0;
    uint32_t program_words = 0;
};

class Mapping {
public:
    Mapping() = default;
    Mapping(int fd, uint64_t physical, size_t size)
        : size_(size), data_(mmap(nullptr, size, PROT_READ | PROT_WRITE,
                                 MAP_SHARED, fd, static_cast<off_t>(physical))) {
        if (data_ == MAP_FAILED) error_ = errno;
    }

    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;

    ~Mapping() {
        if (valid()) munmap(data_, size_);
    }

    bool valid() const { return data_ != MAP_FAILED && data_ != nullptr; }
    void* data() const { return data_; }
    int error() const { return error_; }

private:
    size_t size_ = 0;
    void* data_ = MAP_FAILED;
    int error_ = 0;
};

uint32_t readReg(volatile uint32_t* base, uint32_t offset) {
    return base[offset / sizeof(uint32_t)];
}

void writeReg(volatile uint32_t* base, uint32_t offset, uint32_t value) {
    base[offset / sizeof(uint32_t)] = value;
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

void writeReg64(volatile uint32_t* base, uint32_t offset, uint64_t value) {
    writeReg(base, offset, static_cast<uint32_t>(value));
    writeReg(base, offset + sizeof(uint32_t), static_cast<uint32_t>(value >> 32));
}

bool copyToDevice(volatile uint8_t* destination, const uint8_t* source, size_t size) {
    for (size_t offset = 0; offset < size; ++offset) destination[offset] = source[offset];
    std::atomic_thread_fence(std::memory_order_seq_cst);
    for (size_t offset = 0; offset < size; ++offset) {
        if (destination[offset] != source[offset]) return false;
    }
    return true;
}

uint32_t readBigEndian32(const uint8_t* bytes) {
    return (static_cast<uint32_t>(bytes[0]) << 24) |
           (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) |
           static_cast<uint32_t>(bytes[3]);
}

bool hasDdrReservation() {
    char reg_path[256]{};
    char no_map_path[256]{};
    snprintf(reg_path, sizeof(reg_path), "%s/reg", kDdrReservationNode);
    snprintf(no_map_path, sizeof(no_map_path), "%s/no-map", kDdrReservationNode);

    FILE* file = fopen(reg_path, "rb");
    if (!file) return false;
    uint8_t reg[16]{};
    const size_t size = fread(reg, 1, sizeof(reg), file);
    fclose(file);
    return size == sizeof(reg) && access(no_map_path, F_OK) == 0 &&
           readBigEndian32(reg) == 0 && readBigEndian32(reg + 4) == kDdr.physical_base &&
           readBigEndian32(reg + 8) == 0 && readBigEndian32(reg + 12) == kDdr.size;
}

bool rangesOverlap(uint64_t first_start, uint64_t first_end,
                   uint64_t second_start, uint64_t second_end) {
    return first_start < second_end && second_start < first_end;
}

bool loadElf(const char* path, const MemoryRegion& region,
             volatile uint8_t* memory, KernelInfo& info) {
    FILE* file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "ERROR: cannot open ELF %s: %s\n", path, strerror(errno));
        return false;
    }

    Elf32Header header{};
    if (fread(&header, 1, sizeof(header), file) != sizeof(header)) {
        fprintf(stderr, "ERROR: short ELF header\n");
        fclose(file);
        return false;
    }
    if (header.ident[0] != 0x7f || header.ident[1] != 'E' ||
        header.ident[2] != 'L' || header.ident[3] != 'F' ||
        header.ident[4] != 1 || header.ident[5] != 1 ||
        header.machine != kRiscvMachine ||
        header.header_size != sizeof(Elf32Header) ||
        header.program_header_entry_size != sizeof(Elf32ProgramHeader)) {
        fprintf(stderr, "ERROR: expected little-endian RISC-V ELF32\n");
        fclose(file);
        return false;
    }

    bool found_executable = false;
    for (uint16_t index = 0; index < header.program_header_count; ++index) {
        Elf32ProgramHeader segment{};
        const long position = static_cast<long>(header.program_header_offset) +
                              static_cast<long>(index) * header.program_header_entry_size;
        if (fseek(file, position, SEEK_SET) != 0 ||
            fread(&segment, 1, sizeof(segment), file) != sizeof(segment)) {
            fprintf(stderr, "ERROR: invalid program header %u\n", index);
            fclose(file);
            return false;
        }
        if (segment.type != kLoadSegment || segment.memory_size == 0) continue;
        if (segment.file_size > segment.memory_size) {
            fprintf(stderr, "ERROR: PT_LOAD file size exceeds memory size\n");
            fclose(file);
            return false;
        }

        const uint64_t start = segment.virtual_address;
        const uint64_t end = start + segment.memory_size;
        const uint64_t region_end = region.physical_base + region.size;
        if (start < region.physical_base || end > region_end) {
            fprintf(stderr,
                    "ERROR: PT_LOAD [0x%08x,0x%08llx) is outside %s [0x%08llx,0x%08llx)\n",
                    segment.virtual_address, static_cast<unsigned long long>(end),
                    region.name, static_cast<unsigned long long>(region.physical_base),
                    static_cast<unsigned long long>(region_end));
            fclose(file);
            return false;
        }
        const uint64_t registers_start = region.physical_base + region.register_offset;
        const uint64_t registers_end = registers_start + kRegistersPerWarp;
        const uint64_t result_start = region.physical_base + kResultOffset;
        const uint64_t result_end = result_start + sizeof(uint32_t);
        if (rangesOverlap(start, end, registers_start, registers_end) ||
            rangesOverlap(start, end, result_start, result_end)) {
            fprintf(stderr, "ERROR: PT_LOAD overlaps register or result storage\n");
            fclose(file);
            return false;
        }

        uint8_t* bytes = static_cast<uint8_t*>(calloc(segment.memory_size, 1));
        if (!bytes) {
            fprintf(stderr, "ERROR: cannot allocate segment buffer\n");
            fclose(file);
            return false;
        }
        if (fseek(file, segment.offset, SEEK_SET) != 0 ||
            fread(bytes, 1, segment.file_size, file) != segment.file_size) {
            fprintf(stderr, "ERROR: cannot read PT_LOAD payload\n");
            free(bytes);
            fclose(file);
            return false;
        }

        if ((segment.flags & kExecutable) != 0) {
            if ((segment.file_size % sizeof(uint32_t)) != 0 ||
                (segment.memory_size % sizeof(uint32_t)) != 0) {
                fprintf(stderr, "ERROR: executable segment is not word aligned\n");
                free(bytes);
                fclose(file);
                return false;
            }
            const auto* words = reinterpret_cast<const uint32_t*>(bytes);
            for (size_t word = 0; word < segment.file_size / sizeof(uint32_t); ++word) {
                if ((words[word] & 0x3u) != 0x3u) {
                    fprintf(stderr, "ERROR: compressed instruction at word %zu\n", word);
                    free(bytes);
                    fclose(file);
                    return false;
                }
            }
            info.program_physical = start;
            info.program_words = segment.memory_size / sizeof(uint32_t);
            found_executable = true;
        }

        const size_t region_offset = start - region.physical_base;
        if (!copyToDevice(memory + region_offset, bytes, segment.memory_size)) {
            fprintf(stderr, "ERROR: PT_LOAD readback mismatch\n");
            free(bytes);
            fclose(file);
            return false;
        }
        printf("PT_LOAD vaddr=0x%08x filesz=%u memsz=%u readback=OK\n",
               segment.virtual_address, segment.file_size, segment.memory_size);
        free(bytes);
    }
    fclose(file);

    if (!found_executable || info.program_words == 0 ||
        info.program_words > kMaxProgramWords) {
        fprintf(stderr, "ERROR: executable program length is invalid\n");
        return false;
    }
    return true;
}

const MemoryRegion* selectRegion(const char* elf_path) {
    FILE* file = fopen(elf_path, "rb");
    if (!file) return nullptr;
    Elf32Header header{};
    if (fread(&header, 1, sizeof(header), file) != sizeof(header)) {
        fclose(file);
        return nullptr;
    }
    for (uint16_t index = 0; index < header.program_header_count; ++index) {
        Elf32ProgramHeader segment{};
        const long position = static_cast<long>(header.program_header_offset) +
                              static_cast<long>(index) * header.program_header_entry_size;
        if (fseek(file, position, SEEK_SET) != 0 ||
            fread(&segment, 1, sizeof(segment), file) != sizeof(segment)) break;
        if (segment.type != kLoadSegment || (segment.flags & kExecutable) == 0) continue;
        fclose(file);
        if (segment.virtual_address >= kOcm.physical_base &&
            segment.virtual_address < kOcm.physical_base + kOcm.size) return &kOcm;
        if (segment.virtual_address >= kDdr.physical_base &&
            segment.virtual_address < kDdr.physical_base + kDdr.size) return &kDdr;
        return nullptr;
    }
    fclose(file);
    return nullptr;
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    const char* elf_path = getenv("GPGPU_KERNEL_ELF");
    const uint32_t timeout_ms = static_cast<uint32_t>(
        strtoul(getenv("GPGPU_TIMEOUT_MS") ? getenv("GPGPU_TIMEOUT_MS") : "10000",
                nullptr, 10));
    if (!elf_path) {
        fprintf(stderr, "ERROR: GPGPU_KERNEL_ELF is required\n");
        return 1;
    }

    const MemoryRegion* region = selectRegion(elf_path);
    if (!region) {
        fprintf(stderr, "ERROR: ELF executable segment does not select OCM or DDR\n");
        return 1;
    }
    const size_t registers_end = region->register_offset + kRegistersPerWarp;
    const size_t result_end = kResultOffset + sizeof(uint32_t);
    if (registers_end > region->size || result_end > region->size ||
        rangesOverlap(region->register_offset, registers_end,
                      kResultOffset, result_end)) {
        fprintf(stderr, "ERROR: program/register/result layout overlaps or exceeds region\n");
        return 1;
    }
    if (region == &kDdr && !hasDdrReservation()) {
        fprintf(stderr,
                "ERROR: DDR reservation is missing or invalid at %s\n",
                kDdrReservationNode);
        return 1;
    }

    printf("KRIA_FIRST_KERNEL region=%s physical_base=0x%08llx result_offset=0x%08x\n",
           region->name, static_cast<unsigned long long>(region->physical_base),
           kResultOffset);

    const int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        fprintf(stderr, "ERROR: open /dev/mem: %s\n", strerror(errno));
        return 1;
    }

    Mapping scheduler(fd, kSchedulerControlBase, kControlSize);
    Mapping pointers(fd, kSchedulerPointerBase, kControlSize);
    Mapping memory_control(fd, kMemoryControlBase, kControlSize);
    Mapping scheduler_status(fd, kSchedulerStatusBase, kControlSize);
    Mapping memory(fd, region->physical_base, region->size);
    bool mappings_valid = true;
    const auto check_mapping = [&mappings_valid](const char* name, uint64_t physical,
                                                  size_t size, const Mapping& mapping) {
        if (mapping.valid()) {
            printf("mapped %s [0x%08llx,0x%08llx)\n", name,
                   static_cast<unsigned long long>(physical),
                   static_cast<unsigned long long>(physical + size));
        } else {
            fprintf(stderr, "ERROR: mmap %s [0x%08llx,0x%08llx): %s\n", name,
                    static_cast<unsigned long long>(physical),
                    static_cast<unsigned long long>(physical + size),
                    strerror(mapping.error()));
            mappings_valid = false;
        }
    };
    check_mapping("scheduler-control", kSchedulerControlBase, kControlSize, scheduler);
    check_mapping("scheduler-pointers", kSchedulerPointerBase, kControlSize, pointers);
    check_mapping("memory-control", kMemoryControlBase, kControlSize, memory_control);
    check_mapping("scheduler-status", kSchedulerStatusBase, kControlSize,
                  scheduler_status);
    check_mapping(region->name, region->physical_base, region->size, memory);
    if (!mappings_valid) {
        close(fd);
        return 1;
    }

    auto* sched = static_cast<volatile uint32_t*>(scheduler.data());
    auto* ptrs = static_cast<volatile uint32_t*>(pointers.data());
    auto* mem_ctrl = static_cast<volatile uint32_t*>(memory_control.data());
    auto* status_regs = static_cast<volatile uint32_t*>(scheduler_status.data());
    auto* bytes = static_cast<volatile uint8_t*>(memory.data());
    auto* result = reinterpret_cast<volatile uint32_t*>(
        const_cast<volatile uint8_t*>(bytes + kResultOffset));

    KernelInfo kernel{};
    if (!loadElf(elf_path, *region, bytes, kernel)) {
        close(fd);
        return 1;
    }

    auto* initial_regs = reinterpret_cast<volatile uint32_t*>(
        const_cast<volatile uint8_t*>(bytes + region->register_offset));
    for (size_t index = 0; index < kRegistersPerWarp / sizeof(uint32_t); ++index) {
        initial_regs[index] = 0;
    }
    *result = kPoison;
    std::atomic_thread_fence(std::memory_order_seq_cst);

    writeReg(sched, kSchedStart, 0);
    writeReg64(mem_ctrl, kMemoryGlobalBase, region->physical_base);
    writeReg(mem_ctrl, kMemoryApCtrl, kApStart | kAutoRestart);

    const uint64_t registers_physical = region->physical_base + region->register_offset;
    writeReg64(ptrs, kPtrProgram, kernel.program_physical);
    writeReg64(ptrs, kPtrRegs0, registers_physical);
    writeReg64(ptrs, kPtrRegs1, registers_physical);
    writeReg(sched, kSchedProgramLen, kernel.program_words);
    writeReg(sched, kSchedTotalWarps, 1);
    writeReg(sched, kSchedWarpOffset, 0);

    printf("program=0x%08llx words=%u regs=0x%08llx result=0x%08llx poison=0x%08x\n",
           static_cast<unsigned long long>(kernel.program_physical), kernel.program_words,
           static_cast<unsigned long long>(registers_physical),
           static_cast<unsigned long long>(region->physical_base + kResultOffset), kPoison);

    // gpgpu_scheduler's free-running DATAFLOW sub-blocks (programLoader,
    // barrierCoreN, schedulerCore) are each launched exactly ONCE, when the
    // scheduler's own top-level ap_start first deasserts ap_idle - and Vitis
    // HLS captures every scalar argument (start, total_warps, program_len,
    // warp_id_offset) into a read-once register AT THAT EXACT INSTANT, not
    // as a continuously-live wire (confirmed in the generated RTL: e.g.
    // start_val1_read_reg_114 in *_barrierCore.v only updates during the
    // wrapper's one-shot ap_CS_fsm_state1). So start_r MUST be written and
    // latched BEFORE writing the scheduler's own ap_ctrl=ap_start, exactly
    // like total_warps/program_len/warp_id_offset above - writing it after
    // (an earlier version of this test did that, to make "ready" observable
    // first) permanently bakes in start=0, so the launch condition
    // `start && launch_armed` inside barrierCoreN never fires and the
    // scheduler never reaches busy.
    bool start_latched = false;
    for (uint32_t attempt = 0; attempt < kLaunchWriteRetries; ++attempt) {
        writeReg(sched, kSchedStart, 1);
        start_latched = ((readReg(sched, kSchedStart) & 1u) != 0);
        if (start_latched) break;
    }
    if (!start_latched) {
        fprintf(stderr,
                "ERROR: scheduler start_r did not latch after %u attempts "
                "(ap_ctrl=0x%08x start_r=0x%08x)\n",
                kLaunchWriteRetries,
                readReg(sched, kSchedulerApCtrl), readReg(sched, kSchedStart));
        close(fd);
        return 1;
    }

    const auto start_time = std::chrono::steady_clock::now();
    writeReg(sched, kSchedulerApCtrl, kApStart);

    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    printf("control after launch scheduler_ap=0x%08x start_r=0x%08x "
           "memory_ap=0x%08x status=0x%08x\n",
           readReg(sched, kSchedulerApCtrl), readReg(sched, kSchedStart),
           readReg(mem_ctrl, kMemoryApCtrl), readReg(status_regs, kStatusData));

    bool saw_busy = false;
    bool saw_ready = false;
    bool done = false;
    bool fault = false;
    while (true) {
        const uint32_t status = readReg(status_regs, kStatusData);
        saw_busy = saw_busy || ((status & kStatusBusy) != 0);
        saw_ready = saw_ready || ((status & kStatusReady) != 0);
        fault = (status & kStatusFault) != 0;
        done = (status & kStatusDone) != 0;
        if (fault || done) break;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time).count();
        if (elapsed > timeout_ms) break;
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    std::atomic_thread_fence(std::memory_order_seq_cst);
    const uint32_t actual = *result;
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_time).count();
        printf("status ready_seen=%d busy_seen=%d done=%d fault=%d elapsed_ms=%lld actual=0x%08x\n",
            saw_ready ? 1 : 0, saw_busy ? 1 : 0, done ? 1 : 0, fault ? 1 : 0,
           static_cast<long long>(elapsed_ms), actual);

    if (!saw_busy && !done && !fault) {
        fprintf(stderr,
                "ERROR: scheduler never entered busy state "
                "(ap_ctrl=0x%08x start_r=0x%08x status=0x%08x)\n",
                readReg(sched, kSchedulerApCtrl),
                readReg(sched, kSchedStart),
                readReg(status_regs, kStatusData));
    }

    writeReg(sched, kSchedStart, 0);
    close(fd);

    if (!done || fault || actual != kExpectedSignature || actual == kPoison) {
        fprintf(stderr,
                "FAIL: KRIA_FIRST_KERNEL expected=0x%08x actual=0x%08x done=%d fault=%d\n",
                kExpectedSignature, actual, done ? 1 : 0, fault ? 1 : 0);
        return 1;
    }

    printf("PASS: KRIA_FIRST_KERNEL expected=0x%08x actual=0x%08x done=1 fault=0\n",
           kExpectedSignature, actual);
    return 0;
}