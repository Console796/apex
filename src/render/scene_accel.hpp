#pragma once

#include "lighting/cpu_accel.hpp"
#include "render/renderer.hpp"

namespace apex::render {

enum class SceneAccelChange { Unchanged, Updated, Rebuilt };

// One instance per expanded shadow caster, in draw/instance order. That ordinal is the
// custom index on both backends; primitive IDs are relative to the draw's index range.
// The optional Context enables the Vulkan backend when ray query is available. CPU queries
// remain available on all devices. Context must outlive this object; sync/rebuild require
// all prior GPU uses to have finished, as with Accel. No concurrent queries during mutation.
class SceneAccel {
  public:
    explicit SceneAccel(Context* context = nullptr);
    ~SceneAccel();
    SceneAccel(const SceneAccel&) = delete;
    SceneAccel& operator=(const SceneAccel&) = delete;

    // Pass DrawList::shadowCasters and DrawList::previews verbatim. No casting predicates here.
    // sync selects UPDATE for transform-only changes; rebuild forces a new top-level scene.
    // Immutable meshes are cached by (vertex buffer, firstIndex, indexCount) and pruned on rebuild.
    // Invalid input preserves the scene; a backend update failure clears it for the next sync.
    Result<SceneAccelChange> sync(std::span<const MeshDraw> casters, std::span<const DrawPreview> previews);
    Result<void> rebuild(std::span<const MeshDraw> casters, std::span<const DrawPreview> previews);

    VkAccelerationStructureKHR tlas() const; // Null for CPU-only or an empty scene.
    std::size_t instanceCount() const;
    std::size_t triangleCount() const; // Includes repeated instances.
    std::size_t blasCount() const;
    bool occluded(Vec3 origin, Vec3 direction, float tmax) const;
    std::optional<lighting::RayHit> closest(Vec3 origin, Vec3 direction, float tmax) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace apex::render
