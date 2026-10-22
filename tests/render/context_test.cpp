#include "render/context.hpp"
#include "render/render_target.hpp"
#include "render/renderer.hpp"
#include "render/resources.hpp"
#include "shared_context.hpp"
#include "render/swapchain.hpp"
#include "render/texture_heap.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <vector>

using namespace apex;
using namespace apex::render;

namespace {

Context* sharedContext() {
    return testing::sharedContext();
}

#define REQUIRE_GPU()                                                                              \
    Context* gpu = sharedContext();                                                                \
    if (!gpu) {                                                                                    \
        MESSAGE("Skipped: no Vulkan 1.3 device");                                                  \
        return;                                                                                    \
    }                                                                                              \
    gpu->validation().clear()

std::vector<std::byte> bytes(std::size_t n, std::uint8_t value) {
    return std::vector<std::byte>(n, std::byte{value});
}

int openFdCount() {
#ifdef __linux__
    std::error_code error;
    int count = 0;
    for (std::filesystem::directory_iterator it("/proc/self/fd", error), end; !error && it != end;
         it.increment(error)) ++count;
    return error ? -1 : count;
#else
    return -1;
#endif
}

} // namespace

TEST_CASE("device selection reports capabilities") {
    REQUIRE_GPU();
    const auto& info = gpu->info();
    MESSAGE("device: " << info.name << " ray_query=" << info.rayQuery
                       << " accel=" << info.accelerationStructure << " validation=" << info.validation);
    CHECK(!info.name.empty());
    CHECK(info.apiVersion >= VK_API_VERSION_1_3);
    CHECK(gpu->instance() != VK_NULL_HANDLE);
    CHECK(gpu->allocator() != nullptr);
}

TEST_CASE("buffers: upload, device address and readback") {
    REQUIRE_GPU();
    auto up = Buffer::create(*gpu, {256, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryUsage::Upload});
    auto down = Buffer::create(*gpu, {256, VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryUsage::Readback});
    auto device = Buffer::create(*gpu, {256,
                                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                        MemoryUsage::GpuOnly});
    REQUIRE(up);
    REQUIRE(down);
    REQUIRE(device);
    CHECK(device->deviceAddress() != 0);
    CHECK(device->mapped() == nullptr);
    for (int i = 0; i < 256; ++i) static_cast<std::uint8_t*>(up->mapped())[i] = static_cast<std::uint8_t>(i);
    up->flush();
    REQUIRE(gpu->oneShot([&](VkCommandBuffer cb) {
        const VkBufferCopy copy{0, 0, 256};
        vkCmdCopyBuffer(cb, up->handle(), device->handle(), 1, &copy);
        VkMemoryBarrier2 barrier = vkStruct<VkMemoryBarrier2>(VK_STRUCTURE_TYPE_MEMORY_BARRIER_2);
        barrier.srcStageMask = barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        VkDependencyInfo dep = vkStruct<VkDependencyInfo>(VK_STRUCTURE_TYPE_DEPENDENCY_INFO);
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cb, &dep);
        vkCmdCopyBuffer(cb, device->handle(), down->handle(), 1, &copy);
    }));
    down->invalidate();
    CHECK(static_cast<std::uint8_t*>(down->mapped())[200] == 200);
    Buffer moved = std::move(*up);
    CHECK(!*up);
    CHECK(moved);
    CHECK(gpu->validation().errorCount() == 0);
}

TEST_CASE("render target clears colour and reads back RGBA8") {
    REQUIRE_GPU();
    auto target = RenderTarget::create(*gpu, 37, 21);
    REQUIRE(target);
    const float clear[4] = {1.0f, 0.5f, 0.0f, 1.0f};
    REQUIRE(gpu->oneShot([&](VkCommandBuffer cb) {
        target->begin(cb, clear);
        target->end(cb);
    }));
    auto image = target->readback();
    REQUIRE(image);
    CHECK(image->width == 37);
    CHECK(image->height == 21);
    REQUIRE(image->pixels.size() == 37u * 21u * 4u);
    for (std::size_t i = 0; i < image->pixels.size(); i += 4) {
        REQUIRE(image->pixels[i] == 255);
        REQUIRE((image->pixels[i + 1] == 127 || image->pixels[i + 1] == 128));
        REQUIRE(image->pixels[i + 2] == 0);
        REQUIRE(image->pixels[i + 3] == 255);
    }
    // A second frame re-uses the target.
    const float blue[4] = {0, 0, 1, 1};
    REQUIRE(gpu->oneShot([&](VkCommandBuffer cb) {
        target->begin(cb, blue);
        target->end(cb);
    }));
    auto second = target->readback();
    REQUIRE(second);
    CHECK(second->pixels[2] == 255);
    CHECK(gpu->validation().errorCount() == 0);
}

TEST_CASE("texture heap: checkerboard default, RGBA8, mips and BC formats") {
    REQUIRE_GPU();
    auto created = TextureHeap::create(*gpu, {256, 4u << 20});
    REQUIRE(created);
    auto& heap = **created;
    CHECK(heap.liveTextures() == 1);
    CHECK(heap.descriptorSet() != VK_NULL_HANDLE);

    // 8x8 RGBA8 with a full mip chain 8,4,2,1.
    const auto m0 = bytes(8 * 8 * 4, 200), m1 = bytes(4 * 4 * 4, 100), m2 = bytes(2 * 2 * 4, 50), m3 = bytes(4, 25);
    const std::array<std::span<const std::byte>, 4> chain{m0, m1, m2, m3};
    auto rgba = heap.upload({8, 8, VK_FORMAT_R8G8B8A8_SRGB}, chain);
    REQUIRE(rgba);
    CHECK(*rgba != kCheckerboardSlot);

    struct Bc {
        VkFormat format;
        std::size_t blockBytes;
    };
    const Bc formats[] = {{VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 8}, {VK_FORMAT_BC2_UNORM_BLOCK, 16},
                          {VK_FORMAT_BC3_SRGB_BLOCK, 16},      {VK_FORMAT_BC4_UNORM_BLOCK, 8},
                          {VK_FORMAT_BC5_UNORM_BLOCK, 16},     {VK_FORMAT_BC6H_UFLOAT_BLOCK, 16},
                          {VK_FORMAT_BC7_UNORM_BLOCK, 16}};
    std::vector<TextureSlot> slots;
    for (const auto& bc : formats) {
        // 10x6 is not a multiple of the block size: 3x2 blocks, then 5x3 -> 2x1, 2x1 -> 1x1 ... .
        const auto level0 = bytes(3 * 2 * bc.blockBytes, 7);
        const auto level1 = bytes(2 * 1 * bc.blockBytes, 7);
        const std::array<std::span<const std::byte>, 2> two{level0, level1};
        auto slot = heap.upload({10, 6, bc.format}, two);
        INFO("format " << bc.format);
        REQUIRE(slot);
        slots.push_back(*slot);
    }
    REQUIRE(heap.flush());
    CHECK(heap.liveTextures() == 2 + std::size(formats));

    // Wrong sizes and unsupported formats are rejected.
    const auto tiny = bytes(3, 0);
    CHECK_FALSE(heap.upload({8, 8, VK_FORMAT_R8G8B8A8_UNORM}, tiny));
    CHECK_FALSE(heap.upload({8, 8, VK_FORMAT_D32_SFLOAT}, tiny));

    // Release recycles the slot only after garbage collection is safe; the slot id is reused.
    heap.release(*rgba);
    heap.collectGarbage();
    CHECK(heap.liveTextures() == 1 + std::size(formats));
    auto again = heap.upload({1, 1, VK_FORMAT_R8G8B8A8_UNORM}, bytes(4, 1));
    REQUIRE(again);
    CHECK(*again == *rgba);
    REQUIRE(heap.flush());
    CHECK(gpu->validation().errorCount() == 0);
}

TEST_CASE("texture heap staging ring grows and flushes for large uploads") {
    REQUIRE_GPU();
    auto created = TextureHeap::create(*gpu, {64, 1u << 16}); // 64 KiB ring.
    REQUIRE(created);
    auto& heap = **created;
    // 128x128 RGBA8 = 64 KiB per upload: ring fills and flushes repeatedly; 256x256 forces growth.
    for (int i = 0; i < 4; ++i) REQUIRE(heap.upload({128, 128, VK_FORMAT_R8G8B8A8_UNORM}, bytes(128 * 128 * 4, 9)));
    REQUIRE(heap.upload({256, 256, VK_FORMAT_R8G8B8A8_UNORM}, bytes(256 * 256 * 4, 9)));
    REQUIRE(heap.flush());
    CHECK(gpu->validation().errorCount() == 0);
}

TEST_CASE("swapchain on a headless surface recreates on resize") {
    REQUIRE_GPU();
    // The shared context is created with the surface extensions when the instance offers them.
    if (!Context::instanceExtensionSupported(VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME) ||
        !Context::instanceExtensionSupported(VK_KHR_SURFACE_EXTENSION_NAME)) {
        MESSAGE("Skipped: VK_EXT_headless_surface unavailable");
        return;
    }
    Context& ctx = *gpu;
    if (!ctx.info().swapchain) {
        MESSAGE("Skipped: device has no swapchain support");
        return;
    }
    VkHeadlessSurfaceCreateInfoEXT si = vkStruct<VkHeadlessSurfaceCreateInfoEXT>(VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT);
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    REQUIRE(vkCreateHeadlessSurfaceEXT(ctx.instance(), &si, nullptr, &surface) == VK_SUCCESS);
    {
        auto swapchain = Swapchain::create(ctx, surface, {64, 48});
        if (!swapchain) {
            MESSAGE("Skipped: " << swapchain.error().what());
        } else {
            int frames = 0;
            auto now = Swapchain::Clock::now();
            SwapchainDepth depth(ctx);
            const auto draw = [&](VkExtent2D size) {
                return (*swapchain)->renderFrame(size, [&](VkCommandBuffer cb, const SwapchainFrame& frame) {
                    const float clear[4] = {0.2f, 0.4f, 0.6f, 1.0f};
                    REQUIRE(depth.begin(cb, frame, clear));
                    depth.end(cb);
                    ++frames;
                }, now);
            };
            for (const VkExtent2D size : {VkExtent2D{64, 48}, VkExtent2D{64, 48}, VkExtent2D{100, 80}, VkExtent2D{33, 17}}) {
                auto status = draw(size);
                REQUIRE(status);
                CHECK(*status == FrameStatus::Presented);
                CHECK((*swapchain)->extent().width == size.width);
            }
            CHECK(frames == 4);
            CHECK((*swapchain)->recreateCount() >= 3);

            // Compare an ordinary resize stream with a handle drag at identical scripted sizes.
            const auto baselineCount = (*swapchain)->recreateCount();
            const int fdsBeforeResize = openFdCount();
            int peakFds = fdsBeforeResize;
            const auto baselineStart = Swapchain::Clock::now();
            for (int step = 1; step <= 256; ++step) {
                REQUIRE(draw({static_cast<std::uint32_t>(64 + step), 80}));
                CHECK((*swapchain)->retiredSwapchainCount() <= 2);
                peakFds = std::max(peakFds, openFdCount());
            }
            const double baselineMs = std::chrono::duration<double, std::milli>(Swapchain::Clock::now() - baselineStart).count();
            CHECK((*swapchain)->recreateCount() == baselineCount + 256);
            CHECK((*swapchain)->forcedRetirementCount() > 0);
            if (fdsBeforeResize >= 0 && peakFds >= 0) CHECK(peakFds <= fdsBeforeResize + 128);

            const auto initial = (*swapchain)->recreateCount();
            const int dragFrames = frames;
            const auto dragStart = Swapchain::Clock::now();
            (*swapchain)->setResizeDragging(true, now);
            for (int step = 1; step <= 200; ++step) {
                now += std::chrono::milliseconds(16);
                const auto before = (*swapchain)->recreateCount();
                const auto started = Swapchain::Clock::now();
                const VkExtent2D size{static_cast<std::uint32_t>(64 + step), 80};
                const auto status = draw(size);
                REQUIRE(status);
                const auto after = (*swapchain)->recreateCount();
                MESSAGE("resize step=" << step << " draw_ms="
                        << std::chrono::duration<double, std::milli>(Swapchain::Clock::now() - started).count()
                        << " recreates=" << after);
                CHECK(after - initial == static_cast<std::uint32_t>(step));
                CHECK(after - before == 1u);
                CHECK(*status == FrameStatus::Presented);
                CHECK(frames - dragFrames == step);
                CHECK((*swapchain)->retiredSwapchainCount() <= 2);
            }
            (*swapchain)->setResizeDragging(false, now);
            REQUIRE(draw({264, 80}));
            CHECK((*swapchain)->recreateCount() == initial + 201);
            CHECK((*swapchain)->extent().width == 264);
            MESSAGE("256-step FD resize: peak_delta=" << (peakFds - fdsBeforeResize)
                    << " unthrottled_ms=" << baselineMs << "; 200-step drag_ms="
                    << std::chrono::duration<double, std::milli>(Swapchain::Clock::now() - dragStart).count()
                    << " recreates=" << (*swapchain)->recreateCount() - initial);
            // A later acquisition and its frame fence retire the old presentation resources.
            for (int retry = 0; retry < 12 && (*swapchain)->hasRetiredSwapchains(); ++retry)
                REQUIRE(draw({264, 80}));
            CHECK_FALSE((*swapchain)->hasRetiredSwapchains());
            ctx.waitIdle(); // The local depth target must outlive the last submitted frame.
        }
    }
    ctx.waitIdle();
    vkDestroySurfaceKHR(ctx.instance(), surface, nullptr);
    CHECK(ctx.validation().errorCount() == 0);
}
