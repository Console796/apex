#include "lighting/cpu_accel.hpp"

#include <embree4/rtcore.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace apex::lighting {
namespace {

struct Device {
    RTCDevice handle = rtcNewDevice(nullptr);
    ~Device() { if (handle) rtcReleaseDevice(handle); }
};

std::shared_ptr<Device> device() {
    static auto shared = std::make_shared<Device>();
    return shared;
}

Result<void> checked(const Device& device) {
    if (!device.handle) return std::unexpected(Error{ErrorCode::Internal, "Embree device creation failed"});
    const auto error = rtcGetDeviceError(device.handle);
    if (error != RTC_ERROR_NONE)
        return std::unexpected(Error{ErrorCode::Internal, "Embree error " + std::to_string(error)});
    return {};
}

RTCRay ray(Vec3 origin, Vec3 direction, float tmax) {
    RTCRay result{};
    result.org_x = origin.x; result.org_y = origin.y; result.org_z = origin.z;
    result.dir_x = direction.x; result.dir_y = direction.y; result.dir_z = direction.z;
    result.tfar = tmax;
    result.mask = 0xFF;
    return result;
}

bool validRay(Vec3 origin, Vec3 direction, float tmax) {
    return std::isfinite(origin.x) && std::isfinite(origin.y) && std::isfinite(origin.z) &&
           std::isfinite(direction.x) && std::isfinite(direction.y) && std::isfinite(direction.z) &&
           glm::dot(direction, direction) > 0 && tmax >= 0;
}

} // namespace

struct CpuBlas::Impl {
    std::shared_ptr<Device> device = lighting::device();
    RTCScene scene = nullptr;
    ~Impl() { if (scene) rtcReleaseScene(scene); }
};

CpuBlas::CpuBlas(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
CpuBlas::~CpuBlas() = default;

Result<std::shared_ptr<CpuBlas>> CpuBlas::build(std::span<const std::array<float, 3>> positions,
                                              std::span<const std::uint32_t> indices) {
    if (positions.empty() || positions.size() > std::numeric_limits<std::uint32_t>::max() ||
        indices.empty() || indices.size() % 3 != 0 ||
        std::ranges::any_of(positions, [](const auto& p) {
            return !std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]);
        }) ||
        std::ranges::any_of(indices, [&](auto index) { return index >= positions.size(); }))
        return std::unexpected(Error{ErrorCode::InvalidArgument, "invalid CPU BLAS triangle list"});
    auto impl = std::make_unique<Impl>();
    if (auto ok = checked(*impl->device); !ok) return std::unexpected(ok.error());
    impl->scene = rtcNewScene(impl->device->handle);
    auto geometry = rtcNewGeometry(impl->device->handle, RTC_GEOMETRY_TYPE_TRIANGLE);
    if (!impl->scene || !geometry) {
        if (geometry) rtcReleaseGeometry(geometry);
        return std::unexpected(Error{ErrorCode::Internal, "Embree triangle allocation failed"});
    }
    auto* vertices = rtcSetNewGeometryBuffer(geometry, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3,
                                            sizeof(positions[0]), positions.size());
    auto* elements = rtcSetNewGeometryBuffer(geometry, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3,
                                            3 * sizeof(std::uint32_t), indices.size() / 3);
    if (vertices && elements) {
        std::memcpy(vertices, positions.data(), positions.size_bytes());
        std::memcpy(elements, indices.data(), indices.size_bytes());
        rtcCommitGeometry(geometry);
        rtcAttachGeometry(impl->scene, geometry);
    }
    rtcReleaseGeometry(geometry);
    if (auto ok = checked(*impl->device); !ok) return std::unexpected(ok.error());
    rtcCommitScene(impl->scene);
    if (auto ok = checked(*impl->device); !ok) return std::unexpected(ok.error());
    return std::shared_ptr<CpuBlas>(new CpuBlas(std::move(impl)));
}

struct CpuAccel::Impl {
    std::shared_ptr<Device> device = lighting::device();
    RTCScene scene = nullptr;
    std::vector<CpuInstance> instances;
    ~Impl() { if (scene) rtcReleaseScene(scene); }
};

CpuAccel::CpuAccel(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
CpuAccel::~CpuAccel() = default;

Result<std::unique_ptr<CpuAccel>> CpuAccel::build(std::span<const CpuInstance> instances) {
    if (instances.size() >= RTC_INVALID_GEOMETRY_ID ||
        std::ranges::any_of(instances, [](const auto& instance) { return !instance.blas; }))
        return std::unexpected(Error{ErrorCode::InvalidArgument, "invalid CPU instance list"});
    auto impl = std::make_unique<Impl>();
    if (auto ok = checked(*impl->device); !ok) return std::unexpected(ok.error());
    impl->scene = rtcNewScene(impl->device->handle);
    if (!impl->scene) return std::unexpected(Error{ErrorCode::Internal, "Embree scene allocation failed"});
    rtcSetSceneFlags(impl->scene, RTC_SCENE_FLAG_DYNAMIC);
    // LOW keeps a two-level structure that supports partial instance updates.
    rtcSetSceneBuildQuality(impl->scene, RTC_BUILD_QUALITY_LOW);
    impl->instances.assign(instances.begin(), instances.end());
    for (std::size_t i = 0; i < instances.size(); ++i) {
        auto geometry = rtcNewGeometry(impl->device->handle, RTC_GEOMETRY_TYPE_INSTANCE);
        if (!geometry) return std::unexpected(Error{ErrorCode::Internal, "Embree instance allocation failed"});
        rtcSetGeometryInstancedScene(geometry, instances[i].blas->impl_->scene);
        rtcSetGeometryTransform(geometry, 0, RTC_FORMAT_FLOAT3X4_ROW_MAJOR, instances[i].transform.data());
        rtcCommitGeometry(geometry);
        rtcAttachGeometryByID(impl->scene, geometry, static_cast<unsigned>(i));
        rtcReleaseGeometry(geometry);
    }
    rtcCommitScene(impl->scene);
    if (auto ok = checked(*impl->device); !ok) return std::unexpected(ok.error());
    return std::unique_ptr<CpuAccel>(new CpuAccel(std::move(impl)));
}

Result<void> CpuAccel::updateInstances(std::span<const CpuInstance> instances) {
    if (instances.size() != impl_->instances.size())
        return std::unexpected(Error{ErrorCode::InvalidArgument, "CPU update requires original instances"});
    for (std::size_t i = 0; i < instances.size(); ++i)
        if (instances[i].blas != impl_->instances[i].blas || instances[i].customIndex != impl_->instances[i].customIndex)
            return std::unexpected(Error{ErrorCode::InvalidArgument, "CPU update requires original instances"});
    for (std::size_t i = 0; i < instances.size(); ++i) {
        if (instances[i].transform == impl_->instances[i].transform) continue;
        auto geometry = rtcGetGeometry(impl_->scene, static_cast<unsigned>(i));
        rtcSetGeometryTransform(geometry, 0, RTC_FORMAT_FLOAT3X4_ROW_MAJOR, instances[i].transform.data());
        rtcCommitGeometry(geometry);
    }
    rtcCommitScene(impl_->scene);
    if (auto ok = checked(*impl_->device); !ok) return ok;
    impl_->instances.assign(instances.begin(), instances.end());
    return {};
}

bool CpuAccel::occluded(Vec3 origin, Vec3 direction, float tmax) const {
    if (!validRay(origin, direction, tmax)) return false;
    auto value = ray(origin, direction, tmax);
    rtcOccluded1(impl_->scene, &value, nullptr);
    return value.tfar < 0;
}

std::optional<RayHit> CpuAccel::closest(Vec3 origin, Vec3 direction, float tmax) const {
    if (!validRay(origin, direction, tmax)) return {};
    RTCRayHit value{};
    value.ray = ray(origin, direction, tmax);
    value.hit.geomID = RTC_INVALID_GEOMETRY_ID;
    std::fill(std::begin(value.hit.instID), std::end(value.hit.instID), RTC_INVALID_GEOMETRY_ID);
    rtcIntersect1(impl_->scene, &value, nullptr);
    if (value.hit.geomID == RTC_INVALID_GEOMETRY_ID) return {};
    return RayHit{value.ray.tfar, impl_->instances[value.hit.instID[0]].customIndex, value.hit.primID};
}

} // namespace apex::lighting
