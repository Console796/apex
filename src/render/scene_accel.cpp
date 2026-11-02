#include "render/scene_accel.hpp"

#include "render/accel.hpp"
#include "render/view_math.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace apex::render {
namespace {

using Key = std::tuple<VkBuffer, std::uint32_t, std::uint32_t>;

struct Instance {
    Key key;
    VkBuffer indices;
    std::shared_ptr<const lighting::TriangleMesh> source;
    lighting::RayTransform transform;
    bool operator==(const Instance&) const = default;
};

Result<std::vector<Instance>> expand(std::span<const MeshDraw> casters, std::span<const DrawPreview> previews) {
    std::vector<Instance> result;
    for (const auto& draw : casters) {
        if (!draw.vertices || !draw.indices || !*draw.vertices || !*draw.indices || !draw.cpuMesh ||
            draw.indexCount == 0 || draw.indexCount % 3 != 0 ||
            std::uint64_t(draw.firstIndex) + draw.indexCount > draw.cpuMesh->indices.size() ||
            (draw.instancedTransforms && draw.previewIndex == 0))
            return std::unexpected(Error{ErrorCode::InvalidArgument, "caster has no valid retained triangle mesh"});
        if (result.size() + std::uint64_t(draw.instanceCount) > 0x1000000u)
            return std::unexpected(Error{ErrorCode::InvalidArgument, "too many scene instances for custom indices"});
        for (std::uint32_t i = 0; i < draw.instanceCount; ++i) {
            DMat4 matrix(1.0);
            DVec3 origin = draw.origin;
            if (draw.previewIndex) {
                const auto index = std::uint64_t(draw.previewIndex) - 1 + (draw.instancedTransforms ? i : 0);
                if (index >= previews.size())
                    return std::unexpected(Error{ErrorCode::InvalidArgument, "caster preview index out of range"});
                matrix = previews[index].matrix;
                origin = previews[index].origin;
            }
            const auto packed = cameraRelativePreview(matrix, origin, DVec3(0));
            lighting::RayTransform transform;
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 4; ++c) {
                    transform[r][c] = packed.rows[r][c];
                    if (!std::isfinite(transform[r][c]))
                        return std::unexpected(Error{ErrorCode::InvalidArgument, "nonfinite caster transform"});
                }
            if (glm::determinant(Mat3(matrix)) == 0)
                return std::unexpected(Error{ErrorCode::InvalidArgument, "singular caster transform"});
            result.push_back({{draw.vertices->handle(), draw.firstIndex, draw.indexCount},
                              draw.indices->handle(), draw.cpuMesh, transform});
        }
    }
    return result;
}

bool sameMesh(const Instance& a, const Instance& b) {
    return a.key == b.key && a.indices == b.indices && a.source == b.source;
}

} // namespace

struct SceneAccel::Impl {
    struct Mesh {
        std::shared_ptr<const lighting::TriangleMesh> source;
        VkBuffer indices;
        std::shared_ptr<lighting::CpuBlas> cpu;
        std::unique_ptr<Accel> gpu;
    };
    Context* context;
    std::map<Key, std::shared_ptr<Mesh>> meshes;
    std::vector<Instance> instances;
    std::unique_ptr<lighting::CpuAccel> cpu;
    std::unique_ptr<Accel> gpu; // Destroy top-level structures before their meshes.

    std::vector<lighting::CpuInstance> cpuInstances(const std::vector<Instance>& input,
                                                  const std::map<Key, std::shared_ptr<Mesh>>& cache) const {
        std::vector<lighting::CpuInstance> result;
        for (std::size_t i = 0; i < input.size(); ++i)
            result.push_back({cache.at(input[i].key)->cpu, input[i].transform, static_cast<std::uint32_t>(i)});
        return result;
    }

    std::vector<AccelInstance> gpuInstances(const std::vector<Instance>& input,
                                           const std::map<Key, std::shared_ptr<Mesh>>& cache) const {
        std::vector<AccelInstance> result;
        for (std::size_t i = 0; i < input.size(); ++i) {
            AccelInstance instance;
            instance.blas = cache.at(input[i].key)->gpu.get();
            instance.customIndex = static_cast<std::uint32_t>(i);
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 4; ++c) instance.transform.matrix[r][c] = input[i].transform[r][c];
            result.push_back(instance);
        }
        return result;
    }

    Result<void> rebuild(std::vector<Instance> input) {
        std::map<Key, std::shared_ptr<Mesh>> cache;
        for (const auto& instance : input) {
            if (const auto found = cache.find(instance.key); found != cache.end()) {
                if (found->second->source != instance.source || found->second->indices != instance.indices)
                    return std::unexpected(Error{ErrorCode::InvalidArgument, "inconsistent caster mesh cache key"});
                continue;
            }
            const auto found = meshes.find(instance.key);
            if (found != meshes.end() && found->second->source == instance.source && found->second->indices == instance.indices) {
                cache.emplace(instance.key, found->second);
                continue;
            }
            const auto [buffer, first, count] = instance.key;
            const auto& source = *instance.source;
            const auto indices = std::span(source.indices).subspan(first, count);
            auto cpuBlas = lighting::CpuBlas::build(source.positions, indices);
            if (!cpuBlas) return std::unexpected(cpuBlas.error());
            auto mesh = std::make_shared<Mesh>();
            mesh->source = instance.source;
            mesh->indices = instance.indices;
            mesh->cpu = std::move(*cpuBlas);
            if (context && context->info().rayQuery) {
                auto blas = Accel::buildBlas(*context, source.positions, indices);
                if (!blas) return std::unexpected(blas.error());
                mesh->gpu = std::move(*blas);
            }
            cache.emplace(instance.key, std::move(mesh));
        }
        auto cpuScene = lighting::CpuAccel::build(cpuInstances(input, cache));
        if (!cpuScene) return std::unexpected(cpuScene.error());
        std::unique_ptr<Accel> gpuScene;
        if (context && context->info().rayQuery && !input.empty()) {
            auto built = Accel::buildTlas(*context, gpuInstances(input, cache));
            if (!built) return std::unexpected(built.error());
            gpuScene = std::move(*built);
        }
        gpu = std::move(gpuScene);
        cpu = std::move(*cpuScene);
        meshes = std::move(cache);
        instances = std::move(input);
        return {};
    }
};

SceneAccel::SceneAccel(Context* context) : impl_(std::make_unique<Impl>()) { impl_->context = context; }
SceneAccel::~SceneAccel() = default;

Result<void> SceneAccel::rebuild(std::span<const MeshDraw> casters, std::span<const DrawPreview> previews) {
    auto input = expand(casters, previews);
    if (!input) return std::unexpected(input.error());
    return impl_->rebuild(std::move(*input));
}

Result<SceneAccelChange> SceneAccel::sync(std::span<const MeshDraw> casters, std::span<const DrawPreview> previews) {
    auto input = expand(casters, previews);
    if (!input) return std::unexpected(input.error());
    if (!impl_->cpu || input->size() != impl_->instances.size() ||
        !std::equal(input->begin(), input->end(), impl_->instances.begin(), sameMesh)) {
        if (auto built = impl_->rebuild(std::move(*input)); !built) return std::unexpected(built.error());
        return SceneAccelChange::Rebuilt;
    }
    if (*input == impl_->instances) return SceneAccelChange::Unchanged;
    const auto failed = [&](const Error& error) -> Result<SceneAccelChange> {
        // A backend may already have mutated. Discard both rather than exposing divergent scenes;
        // the next sync rebuilds them from the caller's snapshot.
        impl_->gpu.reset();
        impl_->cpu.reset();
        impl_->instances.clear();
        impl_->meshes.clear();
        return std::unexpected(error);
    };
    auto updated = impl_->cpu->updateInstances(impl_->cpuInstances(*input, impl_->meshes));
    if (!updated) return failed(updated.error());
    if (impl_->gpu) {
        updated = impl_->gpu->updateInstances(impl_->gpuInstances(*input, impl_->meshes));
        if (!updated) return failed(updated.error());
    }
    impl_->instances = std::move(*input);
    return SceneAccelChange::Updated;
}

VkAccelerationStructureKHR SceneAccel::tlas() const { return impl_->gpu ? impl_->gpu->handle() : VK_NULL_HANDLE; }
std::size_t SceneAccel::instanceCount() const { return impl_->instances.size(); }
std::size_t SceneAccel::blasCount() const { return impl_->meshes.size(); }
std::size_t SceneAccel::triangleCount() const {
    std::size_t count = 0;
    for (const auto& instance : impl_->instances) count += std::get<2>(instance.key) / 3;
    return count;
}
bool SceneAccel::occluded(Vec3 origin, Vec3 direction, float tmax) const {
    return impl_->cpu && impl_->cpu->occluded(origin, direction, tmax);
}
std::optional<lighting::RayHit> SceneAccel::closest(Vec3 origin, Vec3 direction, float tmax) const {
    return impl_->cpu ? impl_->cpu->closest(origin, direction, tmax) : std::nullopt;
}

} // namespace apex::render
