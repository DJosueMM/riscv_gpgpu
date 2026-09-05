#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "system_top/system_top.h"

using namespace riscv_gpgpu_hls;

namespace {

struct MockDevice final : DeviceInterface {
    bool ready = true;
    bool busy = false;
    bool done = false;
    bool fault = false;
    bool enabled = false;

    uint32_t warp_id_offset = 0;
    uint32_t total_warps = 0;
    uint32_t program_len = 0;
    uint32_t start_writes = 0;

    void writeEnable(bool v) override { enabled = v; }
    bool readEnabled() const override { return enabled; }

    void writeWarpIdOffset(uint32_t offset) override { warp_id_offset = offset; }
    void writeTotalWarps(uint32_t n) override { total_warps = n; }
    void writeProgramLen(uint32_t len) override { program_len = len; }
    void writeStart(bool v) override {
        if (!v) return;
        ++start_writes;
        busy = true;
        done = false;
        ready = false;
    }

    bool readBusy() const override { return busy; }
    bool readDone() const override { return done; }
    bool readFault() const override { return fault; }
    bool readReady() const override { return ready && !busy && !fault; }
    uint64_t readInstructionsRetired() const override { return 0; }
};

std::vector<std::unique_ptr<DeviceInterface>> makeMockDevices(MockDevice*& d0,
                                                               MockDevice*& d1) {
    auto dev0 = std::make_unique<MockDevice>();
    auto dev1 = std::make_unique<MockDevice>();
    d0 = dev0.get();
    d1 = dev1.get();

    std::vector<std::unique_ptr<DeviceInterface>> devices;
    devices.push_back(std::move(dev0));
    devices.push_back(std::move(dev1));
    return devices;
}

}  // namespace

TEST(SystemTopHlsControl, ConfigureBlockedWhenNotReady) {
    MockDevice* d0 = nullptr;
    MockDevice* d1 = nullptr;
    auto devices = makeMockDevices(d0, d1);
    d1->ready = false;

    SystemTopHLS top({.num_devices = 2}, std::move(devices));

    EXPECT_FALSE(top.configureKernelIfReady(/*total_warps=*/4, /*program_len=*/16));
    EXPECT_EQ(d0->start_writes, 0u);
    EXPECT_EQ(d1->start_writes, 0u);
}

TEST(SystemTopHlsControl, ReadyEnableConfigureStartSequence) {
    MockDevice* d0 = nullptr;
    MockDevice* d1 = nullptr;
    auto devices = makeMockDevices(d0, d1);

    SystemTopHLS top({.num_devices = 2}, std::move(devices));

    ASSERT_TRUE(top.configureKernelIfReady(/*total_warps=*/3, /*program_len=*/21));
    EXPECT_TRUE(d0->readEnabled());
    EXPECT_TRUE(d1->readEnabled());
    EXPECT_EQ(d0->warp_id_offset, 0u);
    EXPECT_EQ(d1->warp_id_offset, 2u);
    EXPECT_EQ(d0->total_warps, 2u);
    EXPECT_EQ(d1->total_warps, 1u);
    EXPECT_EQ(d0->program_len, 21u);
    EXPECT_EQ(d1->program_len, 21u);

    ASSERT_TRUE(top.startConfiguredKernel());
    EXPECT_EQ(d0->start_writes, 1u);
    EXPECT_EQ(d1->start_writes, 1u);
    EXPECT_TRUE(d0->readBusy());
    EXPECT_TRUE(d1->readBusy());
}

TEST(SystemTopHlsControl, StartFailsIfDeviceLosesEnableAfterConfig) {
    MockDevice* d0 = nullptr;
    MockDevice* d1 = nullptr;
    auto devices = makeMockDevices(d0, d1);

    SystemTopHLS top({.num_devices = 2}, std::move(devices));

    ASSERT_TRUE(top.configureKernelIfReady(/*total_warps=*/2, /*program_len=*/8));
    d1->enabled = false;

    EXPECT_FALSE(top.startConfiguredKernel());
    EXPECT_EQ(d0->start_writes, 0u);
    EXPECT_EQ(d1->start_writes, 0u);
}
