#pragma once

// Synchronous, opaque triangle BLAS and updateable TLAS. All allocations use Buffer/VMA.
// The Context must outlive these objects; BLAS objects must outlive TLAS uses that reference them.
// Builds run on the Context's graphics queue and wait for completion. Main thread only;
// callers must finish GPU uses before destroying an Accel.

#include "render/resources.hpp"

#include <array>
#include <memory>
#include <span>

namespace apex::render {

class Accel;

struct AccelInstance {
    const Accel* blas = nullptr;
    // Vulkan row-major 3x4 object-to-world transform. Must be invertible.
    VkTransformMatrixKHR transform{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}};
    std::uint32_t customIndex = 0; // 24 bits, available to the ray-query shader.
    std::uint8_t mask = 0xFF;
};

class Accel {
  public:
    // Positions are tightly packed xyz floats; indices are uint32 triangle lists. Inputs are
    // uploaded internally and may be released on return. Empty geometry is rejected.
    static Result<std::unique_ptr<Accel>> buildBlas(Context& context,
        std::span<const std::array<float, 3>> positions, std::span<const std::uint32_t> indices);
    static Result<std::unique_ptr<Accel>> buildTlas(Context& context, std::span<const AccelInstance> instances);

    // In-place Vulkan UPDATE build. The nonzero instance count must match the original build;
    // transforms, custom indices, masks and BLAS references may change. No concurrent GPU use is allowed.
    Result<void> updateInstances(std::span<const AccelInstance> instances);

    ~Accel();
    Accel(const Accel&) = delete;
    Accel& operator=(const Accel&) = delete;

    VkAccelerationStructureKHR handle() const { return handle_; } // TLAS descriptor binding.
    VkDeviceAddress deviceAddress() const { return address_; }

  private:
    Accel(Context& context, VkAccelerationStructureTypeKHR type) : context_(context), type_(type) {}
    Result<void> build(const VkAccelerationStructureGeometryKHR& geometry, std::uint32_t count, bool update);
    Result<void> buildInstances(std::span<const AccelInstance> instances, bool update);

    Context& context_;
    VkAccelerationStructureTypeKHR type_;
    Buffer storage_;
    VkAccelerationStructureKHR handle_ = VK_NULL_HANDLE;
    VkDeviceAddress address_ = 0;
    std::uint32_t instanceCount_ = 0;
};

} // namespace apex::render
