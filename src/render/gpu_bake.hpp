#pragma once

#include "lighting/baker.hpp"
#include "render/context.hpp"

#include <memory>

namespace apex::render {

class GpuBakeKernel final : public lighting::BakeKernel {
public:
    static Result<std::shared_ptr<GpuBakeKernel>> create(
        Context& context, const lighting::BakeScene& scene, lighting::BakerOptions options);
    ~GpuBakeKernel() override;

    GpuBakeKernel(const GpuBakeKernel&) = delete;
    GpuBakeKernel& operator=(const GpuBakeKernel&) = delete;

    void run(unsigned level, std::span<const lighting::BakeWork> work,
             const std::vector<std::vector<Vec3>>* source, bool sourceChanged,
             std::span<Vec3> sums) override;

private:
    struct Impl;
    explicit GpuBakeKernel(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace apex::render
