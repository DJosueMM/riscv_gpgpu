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
constexpr size_t kControlSize = 0x1000;

constexpr uint32_t kSchedProgramLen = 0x10;
constexpr uint32_t kSchedTotalWarps = 0x18;
constexpr uint32_t kSchedWarpOffset = 0x20;
constexpr uint32_t kSchedStart = 0x28;
constexpr uint32_t kSchedBusy = 0x30;
constexpr uint32_t kSchedDone = 0x40;
constexpr uint32_t kSchedFault = 0x50;

constexpr uint32_t kPtrProgram = 0x10;
constexpr uint32_t kPtrRegs0 = 0x1c;
constexpr uint32_t kPtrRegs1 = 0x28;

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
constexpr const char* kDdrReservation = "memmap=64M$0x60000000";

struct KernelInfo {
    uint64_t program_physical = 0;
    uint32_t program_words = 0;
};

class Mapping {
public:
    Mapping() = default;
    Mapping(int fd, uint64_t physical, size_t size)
        : size_(size), data_(mmap(nullptr, size, PROT_READ | PROT_WRITE,
                                 MAP_SHARED, fd, static_cast<off_t>(physical))) {}

    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;

    ~Mapping() {
        if (valid()) munmap(data_, size_);
    }

    bool valid() const { return data_ != MAP_FAILED && data_ != nullptr; }
    void* data() const { return data_; }

private:
    size_t size_ = 0;
    void* data_ = MAP_FAILED;
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

bool fileContains(const char* path, const char* needle) {
    FILE* file = fopen(path, "r");
    if (!file) return false;
    char contents[4096]{};
    const size_t size = fread(contents, 1, sizeof(contents) - 1, file);
    fclose(file);
    contents[size] = '\0';
    return strstr(contents, needle) != nullptr;
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
    if (region == &kDdr && !fileContains("/proc/cmdline", kDdrReservation)) {
        fprintf(stderr,
                "ERROR: DDR is not reserved; expected %s in /proc/cmdline\n",
                kDdrReservation);
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
    Mapping memory(fd, region->physical_base, region->size);
    if (!scheduler.valid() || !pointers.valid() || !memory_control.valid() ||
        !memory.valid()) {
        fprintf(stderr, "ERROR: mmap failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    auto* sched = static_cast<volatile uint32_t*>(scheduler.data());
    auto* ptrs = static_cast<volatile uint32_t*>(pointers.data());
    auto* mem_ctrl = static_cast<volatile uint32_t*>(memory_control.data());
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

    const auto start_time = std::chrono::steady_clock::now();
    writeReg(sched, kSchedStart, 1);

    bool saw_busy = false;
    bool done = false;
    bool fault = false;
    while (true) {
        saw_busy = saw_busy || ((readReg(sched, kSchedBusy) & 1u) != 0);
        fault = (readReg(sched, kSchedFault) & 1u) != 0;
        done = (readReg(sched, kSchedDone) & 1u) != 0;
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
    printf("status busy_seen=%d done=%d fault=%d elapsed_ms=%lld actual=0x%08x\n",
           saw_busy ? 1 : 0, done ? 1 : 0, fault ? 1 : 0,
           static_cast<long long>(elapsed_ms), actual);

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