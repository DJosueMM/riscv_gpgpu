// fpga_driver.cpp - ARM↔FPGA userspace driver implementation (T051)

#include "fpga_driver.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace riscv_gpgpu {
namespace fpga {

namespace {
constexpr size_t kAllocAlign = 16;

// Instruction memory occupies the bottom of the aperture; device data
// buffers are allocated above this watermark so kernel code is never
// overwritten by gpgpuMalloc().
constexpr uint64_t kDataHeapOffset = 16ULL * 1024 * 1024;  // 16 MiB
} // namespace

FpgaDriver::~FpgaDriver() { close(); }

FpgaDriver& FpgaDriver::instance() {
    static FpgaDriver driver;
    return driver;
}

bool FpgaDriver::open(const FpgaDriverConfig& config) {
    close();

    reg_fd_ = ::open(config.reg_dev_path.c_str(), O_RDWR | O_SYNC);
    if (reg_fd_ < 0) {
        std::cerr << "[fpga_driver] cannot open register device "
                  << config.reg_dev_path << ": " << std::strerror(errno) << "\n";
        return false;
    }
    void* reg_map = ::mmap(nullptr, kRegBlockSize, PROT_READ | PROT_WRITE,
                           MAP_SHARED, reg_fd_,
                           static_cast<off_t>(config.reg_map_offset));
    if (reg_map == MAP_FAILED) {
        std::cerr << "[fpga_driver] mmap of register block failed: "
                  << std::strerror(errno) << "\n";
        close();
        return false;
    }
    regs_ = static_cast<volatile uint32_t*>(reg_map);

    mem_fd_ = ::open(config.mem_dev_path.c_str(), O_RDWR | O_SYNC);
    if (mem_fd_ < 0) {
        std::cerr << "[fpga_driver] cannot open memory device "
                  << config.mem_dev_path << ": " << std::strerror(errno) << "\n";
        close();
        return false;
    }
    void* mem_map = ::mmap(nullptr, config.mem_map_size, PROT_READ | PROT_WRITE,
                           MAP_SHARED, mem_fd_,
                           static_cast<off_t>(config.mem_map_offset));
    if (mem_map == MAP_FAILED) {
        std::cerr << "[fpga_driver] mmap of memory aperture failed: "
                  << std::strerror(errno) << "\n";
        close();
        return false;
    }
    mem_ = static_cast<volatile uint8_t*>(mem_map);
    mem_base_ = config.mem_map_offset;
    mem_size_ = config.mem_map_size;
    // Reserve the bottom of the aperture for kernel code, clamped so small
    // apertures (e.g. test windows) still leave room for data buffers.
    next_alloc_ = mem_base_
                + std::min<uint64_t>(kDataHeapOffset, mem_size_ / 2);
    buffers_.clear();

    if (!config.skip_id_check) {
        const uint32_t id = readReg(REG_ID);
        if (id == kDeviceId) {
            reg_map_v2_ = false;
        } else if (id == v2::kDeviceId) {
            reg_map_v2_ = true;
        } else {
            std::cerr << "[fpga_driver] unexpected device ID 0x" << std::hex << id
                      << " (expected 0x" << kDeviceId
                      << " or 0x" << v2::kDeviceId << ")" << std::dec << "\n";
            close();
            return false;
        }
    } else {
        reg_map_v2_ = (readReg(REG_ID) == v2::kDeviceId);
    }

    std::cout << "[fpga_driver] mapped regs via " << config.reg_dev_path
              << ", memory aperture " << (mem_size_ >> 20) << " MiB @ 0x"
              << std::hex << mem_base_ << std::dec << "\n";
    return true;
}

void FpgaDriver::close() {
    if (regs_ != nullptr) {
        ::munmap(const_cast<uint32_t*>(regs_), kRegBlockSize);
        regs_ = nullptr;
    }
    if (mem_ != nullptr) {
        ::munmap(const_cast<uint8_t*>(mem_), mem_size_);
        mem_ = nullptr;
    }
    if (reg_fd_ >= 0) { ::close(reg_fd_); reg_fd_ = -1; }
    if (mem_fd_ >= 0) { ::close(mem_fd_); mem_fd_ = -1; }
    buffers_.clear();
    reg_map_v2_ = false;
    v2_configured_ = false;
    mem_base_ = mem_size_ = next_alloc_ = 0;
}

uint32_t FpgaDriver::readReg(uint32_t offset) const {
    return regs_[offset / sizeof(uint32_t)];
}

void FpgaDriver::writeReg(uint32_t offset, uint32_t value) {
    regs_[offset / sizeof(uint32_t)] = value;
}

bool FpgaDriver::reset() {
    if (reg_map_v2_) {
        writeReg(v2::REG_CTRL, v2::CTRL_RESET);
        v2_configured_ = false;
    } else {
        writeReg(REG_CTRL, CTRL_RESET);
    }
    return waitForStatus(Status::IDLE, 100);
}

void FpgaDriver::start() {
    if (reg_map_v2_) {
        const uint32_t ctrl = readReg(v2::REG_CTRL);
        writeReg(v2::REG_CTRL, ctrl | v2::CTRL_START);
    } else {
        writeReg(REG_CTRL, CTRL_START);
    }
}

Status FpgaDriver::status() const {
    if (!reg_map_v2_) {
        return static_cast<Status>(readReg(REG_STATUS));
    }
    const uint32_t s = readReg(v2::REG_STATUS);
    if (s & v2::STATUS_FAULT) return Status::ERROR;
    if (s & v2::STATUS_BUSY) return Status::RUNNING;
    if (s & v2::STATUS_DONE) return Status::DONE;
    return Status::IDLE;
}

bool FpgaDriver::waitForStatus(Status expected, uint32_t timeout_ms) const {
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(timeout_ms);
    while (status() != expected) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    return true;
}

uint32_t FpgaDriver::readStatusWord() const {
    return reg_map_v2_ ? readReg(v2::REG_STATUS) : readReg(REG_STATUS);
}

bool FpgaDriver::readReadyBit() const {
    if (!reg_map_v2_) return status() == Status::IDLE;
    return (readReg(v2::REG_STATUS) & v2::STATUS_READY) != 0;
}

bool FpgaDriver::readEnabledBit() const {
    if (!reg_map_v2_) return true;
    const uint32_t status_reg = readReg(v2::REG_STATUS);
    if ((status_reg & v2::STATUS_ENABLED) != 0) return true;
    // Fallback for early bring-up wrappers that have CTRL.ENABLE wired but not
    // STATUS.ENABLED yet.
    const uint32_t ctrl_reg = readReg(v2::REG_CTRL);
    return (ctrl_reg & v2::CTRL_ENABLE) != 0;
}

bool FpgaDriver::readPllLockedBit() const {
    if (!reg_map_v2_) return true;
    return (readReg(v2::REG_STATUS) & v2::STATUS_PLL_LOCKED) != 0;
}

void FpgaDriver::writeEnable(bool enable) {
    if (!reg_map_v2_) return;
    uint32_t ctrl = readReg(v2::REG_CTRL);
    if (enable) ctrl |= v2::CTRL_ENABLE;
    else        ctrl &= ~v2::CTRL_ENABLE;
    writeReg(v2::REG_CTRL, ctrl);
}

bool FpgaDriver::configureAndEnableLaunchV2(const FpgaLaunchConfigV2& cfg) {
    if (!reg_map_v2_) return false;
    if (!readReadyBit() || !readPllLockedBit()) return false;

    writeReg(v2::REG_WARP_ID_OFFSET, cfg.warp_id_offset);
    writeReg(v2::REG_TOTAL_WARPS, cfg.total_warps);
    writeReg(v2::REG_PROGRAM_LEN, cfg.program_len);
    writeReg(v2::REG_PC_INIT_LO, static_cast<uint32_t>(cfg.entry_point & 0xffffffffULL));
    writeReg(v2::REG_PC_INIT_HI, static_cast<uint32_t>((cfg.entry_point >> 32) & 0xffffffffULL));
    writeReg(v2::REG_DESC_BASE_LO, static_cast<uint32_t>(cfg.desc_base & 0xffffffffULL));
    writeReg(v2::REG_DESC_BASE_HI, static_cast<uint32_t>((cfg.desc_base >> 32) & 0xffffffffULL));
    writeReg(v2::REG_DESC_STRIDE_BYTES, cfg.desc_stride_bytes);
    writeReg(v2::REG_DESC_COUNT, cfg.desc_count);

    writeEnable(true);
    v2_configured_ = true;
    return true;
}

bool FpgaDriver::startConfiguredLaunchV2() {
    if (!reg_map_v2_ || !v2_configured_) return false;
    if (!readReadyBit() || !readPllLockedBit() || !readEnabledBit()) return false;

    const uint32_t ctrl = readReg(v2::REG_CTRL);
    writeReg(v2::REG_CTRL, ctrl | v2::CTRL_START);
    v2_configured_ = false;
    return true;
}

void FpgaDriver::ringDoorbellV2(uint32_t value) {
    if (!reg_map_v2_) return;
    writeReg(v2::REG_DOORBELL, value);
}

// ── Device memory ─────────────────────────────────────────────────────────────

volatile uint8_t* FpgaDriver::memAt(uint64_t dev_addr, size_t size) const {
    if (mem_ == nullptr) return nullptr;
    if (dev_addr < mem_base_ || dev_addr + size > mem_base_ + mem_size_) {
        std::cerr << "[fpga_driver] device address 0x" << std::hex << dev_addr
                  << " +" << std::dec << size << " outside aperture\n";
        return nullptr;
    }
    return mem_ + (dev_addr - mem_base_);
}

bool FpgaDriver::allocateBuffer(uint64_t& dev_addr, size_t size) {
    if (size == 0) {
        std::cerr << "[fpga_driver] allocateBuffer: size must be > 0\n";
        return false;
    }
    const size_t aligned = ((size + kAllocAlign - 1) / kAllocAlign) * kAllocAlign;
    if (next_alloc_ + aligned > mem_base_ + mem_size_) {
        std::cerr << "[fpga_driver] allocateBuffer: aperture exhausted\n";
        return false;
    }
    dev_addr = next_alloc_;
    next_alloc_ += aligned;
    buffers_[dev_addr] = size;
    return true;
}

bool FpgaDriver::freeBuffer(uint64_t dev_addr) {
    return buffers_.erase(dev_addr) != 0;
}

size_t FpgaDriver::bufferSize(uint64_t dev_addr) const {
    auto it = buffers_.find(dev_addr);
    return (it != buffers_.end()) ? it->second : 0;
}

bool FpgaDriver::copyToDevice(uint64_t dst_dev, const void* src_host, size_t size) {
    auto it = buffers_.find(dst_dev);
    if (it == buffers_.end() || size > it->second) {
        std::cerr << "[fpga_driver] copyToDevice: invalid buffer 0x"
                  << std::hex << dst_dev << std::dec << " size " << size << "\n";
        return false;
    }
    return writeMem(dst_dev, src_host, size);
}

bool FpgaDriver::copyFromDevice(void* dst_host, uint64_t src_dev, size_t size) {
    auto it = buffers_.find(src_dev);
    if (it == buffers_.end() || size > it->second) {
        std::cerr << "[fpga_driver] copyFromDevice: invalid buffer 0x"
                  << std::hex << src_dev << std::dec << " size " << size << "\n";
        return false;
    }
    return readMem(dst_host, src_dev, size);
}

bool FpgaDriver::writeMem(uint64_t dev_addr, const void* src_host, size_t size) {
    volatile uint8_t* dst = memAt(dev_addr, size);
    if (dst == nullptr) return false;
    std::memcpy(const_cast<uint8_t*>(dst), src_host, size);
    return true;
}

bool FpgaDriver::readMem(void* dst_host, uint64_t dev_addr, size_t size) const {
    volatile uint8_t* src = memAt(dev_addr, size);
    if (src == nullptr) return false;
    std::memcpy(dst_host, const_cast<const uint8_t*>(src), size);
    return true;
}

} // namespace fpga
} // namespace riscv_gpgpu
