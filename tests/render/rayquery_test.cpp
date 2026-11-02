#include "render/accel.hpp"
#include "render/test_shaders_spv.hpp"
#include "shared_context.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstring>

using namespace apex;
using namespace apex::render;

namespace {

constexpr std::array<std::array<float, 3>, 4> kQuad{{{-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}}};
constexpr std::array<std::uint32_t, 6> kIndices{0, 1, 2, 0, 2, 3};

struct Ray {
    std::array<float, 4> origin;
    std::array<float, 4> direction{0, 0, -1, 100};
    std::array<std::uint32_t, 4> options{0xFF, 0, 0, 0};
};
struct Hit {
    std::uint32_t hit;
    float distance;
    std::uint32_t customIndex;
    std::uint32_t padding;
};
static_assert(sizeof(Ray) == 48 && sizeof(Hit) == 16);

struct Probe {
    VkDevice device;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkShaderModule shader = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    ~Probe() {
        if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
        if (layout) vkDestroyPipelineLayout(device, layout, nullptr);
        if (shader) vkDestroyShaderModule(device, shader, nullptr);
        if (pool) vkDestroyDescriptorPool(device, pool, nullptr);
        if (setLayout) vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
    }
};

} // namespace

TEST_CASE("render: ray query hits misses instances and TLAS transform update") {
    auto* context = testing::sharedContext();
    if (!context) { MESSAGE("Skipped: no Vulkan device"); return; }
    MESSAGE("Ray-query device: " << context->info().name << "; validation=" << context->info().validation);
    if (!context->info().rayQuery) { MESSAGE("Skipped: ray query is not enabled on this device"); return; }
    REQUIRE(context->info().accelerationStructure);
    context->validation().clear();
    {
        CHECK_FALSE(Accel::buildBlas(*context, {}, kIndices));
        const std::array<std::uint32_t, 3> invalid{0, 1, 4};
        CHECK_FALSE(Accel::buildBlas(*context, kQuad, invalid));
        CHECK_FALSE(Accel::buildTlas(*context, {}));
        auto blas = Accel::buildBlas(*context, kQuad, kIndices);
        REQUIRE(blas);
        CHECK((*blas)->deviceAddress() != 0);
        std::array<AccelInstance, 2> instances;
        instances[0].blas = blas->get();
        instances[0].customIndex = 17;
        instances[0].mask = 1;
        instances[1].blas = blas->get();
        instances[1].customIndex = 29;
        instances[1].mask = 2;
        instances[1].transform.matrix[0][3] = 4;
        instances[1].transform.matrix[2][3] = 2;
        auto tlas = Accel::buildTlas(*context, instances);
        REQUIRE(tlas);
        CHECK_FALSE((*blas)->updateInstances(instances));
        CHECK_FALSE((*tlas)->updateInstances(std::span(instances).first(1)));
        auto badInstances = instances;
        badInstances[0].blas = tlas->get();
        CHECK_FALSE((*tlas)->updateInstances(badInstances));
        badInstances = instances;
        badInstances[0].customIndex = 0x1000000u;
        CHECK_FALSE((*tlas)->updateInstances(badInstances));

        // Off-diagonal rays avoid shared triangle edges. Last ray tests the instance mask.
        const std::array<Ray, 5> rays{{{{0.25f, 0, 10, 0}}, {{2, 0, 10, 0}},
            {{4.25f, 0, 10, 0}}, {{8.25f, 0, 10, 0}}, {{4.25f, 0, 10, 0}, {0, 0, -1, 100}, {1, 0, 0, 0}}}};
        auto input = Buffer::create(*context, {sizeof(rays),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, MemoryUsage::Upload});
        auto output = Buffer::create(*context, {rays.size() * sizeof(Hit),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, MemoryUsage::Readback});
        REQUIRE(input);
        REQUIRE(output);
        std::memcpy(input->mapped(), rays.data(), sizeof(rays));
        input->flush();

        Probe probe{context->device()};
        const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1,
                                                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        auto setLayout = vkStruct<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
        setLayout.bindingCount = 1; setLayout.pBindings = &binding;
        REQUIRE(vkCreateDescriptorSetLayout(probe.device, &setLayout, nullptr, &probe.setLayout) == VK_SUCCESS);
        const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1};
        auto pool = vkStruct<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
        pool.maxSets = 1; pool.poolSizeCount = 1; pool.pPoolSizes = &poolSize;
        REQUIRE(vkCreateDescriptorPool(probe.device, &pool, nullptr, &probe.pool) == VK_SUCCESS);
        auto allocate = vkStruct<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
        allocate.descriptorPool = probe.pool; allocate.descriptorSetCount = 1; allocate.pSetLayouts = &probe.setLayout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        REQUIRE(vkAllocateDescriptorSets(probe.device, &allocate, &set) == VK_SUCCESS);
        const auto handle = (*tlas)->handle();
        auto acceleration = vkStruct<VkWriteDescriptorSetAccelerationStructureKHR>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR);
        acceleration.accelerationStructureCount = 1; acceleration.pAccelerationStructures = &handle;
        auto write = vkStruct<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
        write.pNext = &acceleration; write.dstSet = set; write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        vkUpdateDescriptorSets(probe.device, 1, &write, 0, nullptr);
        auto shader = vkStruct<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
        shader.codeSize = sizeof(shaders::rayquery_probe_comp); shader.pCode = shaders::rayquery_probe_comp;
        REQUIRE(vkCreateShaderModule(probe.device, &shader, nullptr, &probe.shader) == VK_SUCCESS);
        const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 2 * sizeof(VkDeviceAddress)};
        auto layout = vkStruct<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
        layout.setLayoutCount = 1; layout.pSetLayouts = &probe.setLayout;
        layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &range;
        REQUIRE(vkCreatePipelineLayout(probe.device, &layout, nullptr, &probe.layout) == VK_SUCCESS);
        auto pipeline = vkStruct<VkComputePipelineCreateInfo>(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
        pipeline.layout = probe.layout;
        pipeline.stage = vkStruct<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
        pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; pipeline.stage.module = probe.shader; pipeline.stage.pName = "main";
        REQUIRE(vkCreateComputePipelines(probe.device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &probe.pipeline) == VK_SUCCESS);

        const auto trace = [&] {
            REQUIRE(context->oneShot([&](VkCommandBuffer commands) {
                vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, probe.pipeline);
                vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, probe.layout, 0, 1, &set, 0, nullptr);
                const std::array<VkDeviceAddress, 2> addresses{input->deviceAddress(), output->deviceAddress()};
                vkCmdPushConstants(commands, probe.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(addresses), addresses.data());
                vkCmdDispatch(commands, static_cast<std::uint32_t>(rays.size()), 1, 1);
                auto barrier = vkStruct<VkMemoryBarrier2>(VK_STRUCTURE_TYPE_MEMORY_BARRIER_2);
                barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
                barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT; barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
                auto dep = vkStruct<VkDependencyInfo>(VK_STRUCTURE_TYPE_DEPENDENCY_INFO);
                dep.memoryBarrierCount = 1; dep.pMemoryBarriers = &barrier;
                vkCmdPipelineBarrier2(commands, &dep);
            }));
            output->invalidate();
        };
        const auto check = [&](std::size_t ray, bool hit, float distance, std::uint32_t index) {
            CAPTURE(ray);
            const auto& value = static_cast<const Hit*>(output->mapped())[ray];
            CHECK(value.hit == (hit ? 1u : 0u));
            CHECK(value.distance == doctest::Approx(distance));
            CHECK(value.customIndex == index);
        };
        trace();
        check(0, true, 10, 17);
        check(1, false, -1, 0xFFFFFFFFu);
        check(2, true, 8, 29);
        check(3, false, -1, 0xFFFFFFFFu);
        check(4, false, -1, 0xFFFFFFFFu);
        instances[1].transform.matrix[0][3] = 8;
        instances[1].transform.matrix[2][3] = 4;
        REQUIRE((*tlas)->updateInstances(instances));
        CHECK((*tlas)->handle() == handle); // The descriptor remains valid after refit.
        trace();
        check(0, true, 10, 17);
        check(1, false, -1, 0xFFFFFFFFu);
        check(2, false, -1, 0xFFFFFFFFu);
        check(3, true, 6, 29);
        check(4, false, -1, 0xFFFFFFFFu);
    }
    for (const auto& error : context->validation().errors()) MESSAGE(error);
    CHECK(context->validation().errorCount() == 0);
    CHECK(context->validation().warningCount() == 0);
}
