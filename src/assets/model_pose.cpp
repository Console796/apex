#include "assets/model_pose.hpp"

#include <mdlpp/mdlpp.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>

namespace apex::assets {
namespace {
// studiohdr_t offsets (versions 44-49).
constexpr std::size_t kHeaderAnimCount = 180;
constexpr std::size_t kHeaderAnimIndex = 184;
constexpr std::size_t kAnimDescSize = 100;

constexpr std::uint8_t kRawPos = 0x01;
constexpr std::uint8_t kRawRot = 0x02;
constexpr std::uint8_t kAnimPos = 0x04;
constexpr std::uint8_t kAnimRot = 0x08;
constexpr std::uint8_t kDelta = 0x10;
constexpr std::uint8_t kRawRot2 = 0x20;
constexpr std::int32_t kAnimDescDelta = 0x0004;

class Reader {
public:
    explicit Reader(std::span<const std::byte> bytes) : bytes_(bytes) {}

    template <typename T>
    std::optional<T> read(std::int64_t offset) const {
        if (offset < 0 || static_cast<std::uint64_t>(offset) + sizeof(T) > bytes_.size()) return std::nullopt;
        T value;
        std::memcpy(&value, bytes_.data() + offset, sizeof(T));
        return value;
    }

private:
    std::span<const std::byte> bytes_;
};

float halfToFloat(std::uint16_t h) {
    const std::uint32_t sign = (h >> 15) & 1u;
    const std::uint32_t exponent = (h >> 10) & 0x1Fu;
    const std::uint32_t mantissa = h & 0x3FFu;
    float value;
    if (exponent == 0) {
        value = std::ldexp(static_cast<float>(mantissa), -24);
    } else if (exponent == 31) {
        value = mantissa ? std::nanf("") : INFINITY;
    } else {
        value = std::ldexp(static_cast<float>(mantissa | 0x400u), static_cast<int>(exponent) - 25);
    }
    return sign ? -value : value;
}

struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
};

Quat quatFromEuler(float roll, float pitch, float yaw) {
    const float sr = std::sin(roll * 0.5f), cr = std::cos(roll * 0.5f);
    const float sp = std::sin(pitch * 0.5f), cp = std::cos(pitch * 0.5f);
    const float sy = std::sin(yaw * 0.5f), cy = std::cos(yaw * 0.5f);
    return {sr * cp * cy - cr * sp * sy, cr * sp * cy + sr * cp * sy, cr * cp * sy - sr * sp * cy,
            cr * cp * cy + sr * sp * sy};
}

BoneMatrix fromQuatPos(const Quat& q, const std::array<float, 3>& p) {
    const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    return {1 - 2 * yy - 2 * zz, 2 * q.x * q.y - 2 * q.w * q.z, 2 * q.x * q.z + 2 * q.w * q.y, p[0],
            2 * q.x * q.y + 2 * q.w * q.z, 1 - 2 * xx - 2 * zz, 2 * q.y * q.z - 2 * q.w * q.x, p[1],
            2 * q.x * q.z - 2 * q.w * q.y, 2 * q.y * q.z + 2 * q.w * q.x, 1 - 2 * xx - 2 * yy, p[2]};
}

BoneMatrix multiply(const BoneMatrix& a, const BoneMatrix& b) {
    BoneMatrix out{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            out[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] +
                             (c == 3 ? a[r * 4 + 3] : 0.0f);
        }
    }
    return out;
}

// mstudioanimvalue_t run-length data at `offset`, evaluated at frame 0.
float animValueAtFrameZero(const Reader& in, std::int64_t offset) {
    std::uint32_t frame = 0;
    for (int guard = 0; guard < 4096; ++guard) {
        const auto header = in.read<std::uint16_t>(offset);
        if (!header) return 0.0f;
        const auto valid = static_cast<std::uint8_t>(*header & 0xFF);
        const auto total = static_cast<std::uint8_t>(*header >> 8);
        if (total == 0) return 0.0f;
        if (total <= frame) {
            frame -= total;
            offset += (static_cast<std::int64_t>(valid) + 1) * 2;
            continue;
        }
        const std::uint32_t index = valid > frame ? frame : valid;
        const auto value = in.read<std::int16_t>(offset + (static_cast<std::int64_t>(index) + 1) * 2);
        return value ? static_cast<float>(*value) : 0.0f;
    }
    return 0.0f;
}

bool nearIdentity(const BoneMatrix& m) {
    constexpr float kEpsilon = 1e-4f;
    constexpr BoneMatrix identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    for (std::size_t i = 0; i < 12; ++i)
        if (std::abs(m[i] - identity[i]) > kEpsilon) return false;
    return true;
}

} // namespace

std::vector<BoneMatrix> sequenceZeroSkinning(const mdlpp::MDL::MDL& mdl, std::span<const std::byte> mdlBytes) {
    if (mdlBytes.empty() || mdl.bones.empty() || mdl.bones.size() > 256 || mdl.sequences.empty()) return {};
    const auto& sequence = mdl.sequences.front();
    if (sequence.animIndices.empty() || sequence.animIndices.front() < 0) return {};
    const Reader in(mdlBytes);

    const auto animCount = in.read<std::int32_t>(kHeaderAnimCount);
    const auto animBase = in.read<std::int32_t>(kHeaderAnimIndex);
    const auto animIndex = static_cast<std::int32_t>(sequence.animIndices.front());
    if (!animCount || !animBase || animIndex >= *animCount) return {};
    const std::int64_t desc = static_cast<std::int64_t>(*animBase) + static_cast<std::int64_t>(animIndex) * kAnimDescSize;

    const auto descFlags = in.read<std::int32_t>(desc + 12);
    const auto animBlock = in.read<std::int32_t>(desc + 52);
    const auto animOffset = in.read<std::int32_t>(desc + 56);
    const auto sectionOffset = in.read<std::int32_t>(desc + 80);
    const auto sectionFrames = in.read<std::int32_t>(desc + 84);
    if (!descFlags || !animBlock || !animOffset || !sectionOffset || !sectionFrames) return {};
    if (*descFlags & kAnimDescDelta) return {};

    std::int64_t data = desc + *animOffset;
    std::int32_t block = *animBlock;
    if (*sectionFrames != 0) {
        const auto sectionBlock = in.read<std::int32_t>(desc + *sectionOffset);
        const auto sectionData = in.read<std::int32_t>(desc + *sectionOffset + 4);
        if (!sectionBlock || !sectionData) return {};
        block = *sectionBlock;
        data = desc + *sectionData;
    }
    if (block != 0) return {}; // external .ani block
    if (*animOffset == 0 && *sectionFrames == 0) return {};

    const std::size_t boneCount = mdl.bones.size();
    struct Local {
        Quat rotation;
        std::array<float, 3> position{};
    };
    std::vector<Local> locals(boneCount);
    for (std::size_t i = 0; i < boneCount; ++i) {
        const auto& bone = mdl.bones[i];
        locals[i].rotation = {bone.rotationQuat[0], bone.rotationQuat[1], bone.rotationQuat[2], bone.rotationQuat[3]};
        locals[i].position = {bone.position[0], bone.position[1], bone.position[2]};
    }

    std::int64_t entry = data;
    for (int guard = 0; guard < 4096; ++guard) {
        const auto boneIndex = in.read<std::uint8_t>(entry);
        const auto flags = in.read<std::uint8_t>(entry + 1);
        const auto next = in.read<std::int16_t>(entry + 2);
        if (!boneIndex || !flags || !next || *boneIndex == 255) break;
        if (*boneIndex < boneCount && !(*flags & kDelta)) {
            const auto& bone = mdl.bones[*boneIndex];
            auto& local = locals[*boneIndex];
            std::int64_t cursor = entry + 4;
            const bool hasRot = (*flags & (kRawRot | kRawRot2)) != 0;
            if (*flags & kRawRot2) {
                const auto packed = in.read<std::uint64_t>(cursor);
                if (!packed) return {};
                const auto x = static_cast<float>(static_cast<std::int64_t>(*packed & 0x1FFFFF) - 1048576) * (1.0f / 1048576.5f);
                const auto y = static_cast<float>(static_cast<std::int64_t>((*packed >> 21) & 0x1FFFFF) - 1048576) * (1.0f / 1048576.5f);
                const auto z = static_cast<float>(static_cast<std::int64_t>((*packed >> 42) & 0x1FFFFF) - 1048576) * (1.0f / 1048576.5f);
                float w = std::sqrt(std::max(0.0f, 1.0f - x * x - y * y - z * z));
                if ((*packed >> 63) & 1u) w = -w;
                local.rotation = {x, y, z, w};
                cursor += 8;
            } else if (*flags & kRawRot) {
                const auto x16 = in.read<std::uint16_t>(cursor);
                const auto y16 = in.read<std::uint16_t>(cursor + 2);
                const auto z16 = in.read<std::uint16_t>(cursor + 4);
                if (!x16 || !y16 || !z16) return {};
                const float x = (static_cast<int>(*x16) - 32768) * (1.0f / 32768.0f);
                const float y = (static_cast<int>(*y16) - 32768) * (1.0f / 32768.0f);
                const float z = (static_cast<int>(*z16 & 0x7FFF) - 16384) * (1.0f / 16384.0f);
                float w = std::sqrt(std::max(0.0f, 1.0f - x * x - y * y - z * z));
                if (*z16 & 0x8000) w = -w;
                local.rotation = {x, y, z, w};
                cursor += 6;
            }
            if (*flags & kRawPos) {
                for (int axis = 0; axis < 3; ++axis) {
                    const auto half = in.read<std::uint16_t>(cursor + axis * 2);
                    if (!half) return {};
                    local.position[axis] = halfToFloat(*half);
                }
                cursor += 6;
            }
            if (*flags & kAnimRot) {
                const std::int64_t base = cursor;
                float angles[3];
                for (int axis = 0; axis < 3; ++axis) {
                    const auto offset = in.read<std::int16_t>(base + axis * 2);
                    if (!offset) return {};
                    angles[axis] = bone.rotationEuler[axis];
                    if (*offset > 0)
                        angles[axis] += animValueAtFrameZero(in, base + *offset) * bone.rotationScale[axis];
                }
                if (!hasRot) local.rotation = quatFromEuler(angles[0], angles[1], angles[2]);
                cursor += 6;
            }
            if (*flags & kAnimPos) {
                const std::int64_t base = cursor;
                for (int axis = 0; axis < 3; ++axis) {
                    const auto offset = in.read<std::int16_t>(base + axis * 2);
                    if (!offset) return {};
                    local.position[axis] = bone.position[axis];
                    if (*offset > 0)
                        local.position[axis] += animValueAtFrameZero(in, base + *offset) * bone.positionScale[axis];
                }
            }
        }
        if (*next == 0) break;
        entry += *next;
    }

    std::vector<BoneMatrix> world(boneCount);
    std::vector<BoneMatrix> skinning(boneCount);
    bool changed = false;
    for (std::size_t i = 0; i < boneCount; ++i) {
        const auto& bone = mdl.bones[i];
        auto& rot = locals[i].rotation;
        const float length = std::sqrt(rot.x * rot.x + rot.y * rot.y + rot.z * rot.z + rot.w * rot.w);
        if (!(length > 1e-6f) || !std::isfinite(length)) return {};
        rot = {rot.x / length, rot.y / length, rot.z / length, rot.w / length};
        for (const float v : locals[i].position)
            if (!std::isfinite(v)) return {};
        const auto local = fromQuatPos(rot, locals[i].position);
        // Parents precede children in a studio bone table; anything else is malformed.
        if (bone.parent >= static_cast<std::int32_t>(i)) return {};
        world[i] = bone.parent < 0 ? local : multiply(world[static_cast<std::size_t>(bone.parent)], local);

        BoneMatrix poseToBone{};
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c) poseToBone[r * 4 + c] = bone.poseToBose[r][c];
        skinning[i] = multiply(world[i], poseToBone);
        for (const float v : skinning[i])
            if (!std::isfinite(v)) return {};
        changed = changed || !nearIdentity(skinning[i]);
    }
    if (!changed) return {};
    return skinning;
}

} // namespace apex::assets
