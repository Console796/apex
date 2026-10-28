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

// mstudioanimvalue_t run-length data at `offset`.
std::optional<float> animValue(const Reader& in, std::int64_t offset, std::uint32_t frame) {
    for (int guard = 0; guard < 4096; ++guard) {
        const auto header = in.read<std::uint16_t>(offset);
        if (!header) return std::nullopt;
        const auto valid = static_cast<std::uint8_t>(*header & 0xFF);
        const auto total = static_cast<std::uint8_t>(*header >> 8);
        if (total == 0 || valid == 0 || valid > total) return std::nullopt;
        if (total <= frame) {
            frame -= total;
            offset += (static_cast<std::int64_t>(valid) + 1) * 2;
            continue;
        }
        const std::uint32_t index = valid > frame ? frame : valid - 1u;
        const auto value = in.read<std::int16_t>(offset + (static_cast<std::int64_t>(index) + 1) * 2);
        return value ? std::optional(static_cast<float>(*value)) : std::nullopt;
    }
    return std::nullopt;
}

bool nearIdentity(const BoneMatrix& m) {
    constexpr float kEpsilon = 1e-4f;
    constexpr BoneMatrix identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    for (std::size_t i = 0; i < 12; ++i)
        if (std::abs(m[i] - identity[i]) > kEpsilon) return false;
    return true;
}

struct Local { Quat rotation; std::array<float, 3> position{}; };

std::optional<std::vector<Local>> sampleLocals(const mdlpp::MDL::MDL& mdl,
    std::span<const std::byte> mdlBytes, int sequenceIndex, std::uint32_t frame) {
    if (mdlBytes.empty() || mdl.bones.empty() || mdl.bones.size() > 256 || sequenceIndex < 0 || static_cast<std::size_t>(sequenceIndex) >= mdl.sequences.size()) return {};
    const auto& sequence = mdl.sequences[static_cast<std::size_t>(sequenceIndex)];
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
    if (*sectionFrames < 0) return {};
    if (*sectionFrames != 0) {
        const auto frameCount = in.read<std::int32_t>(desc + 16);
        if (!frameCount || *frameCount <= 0) return {};
        std::uint32_t section = frame / static_cast<std::uint32_t>(*sectionFrames);
        if (*frameCount > *sectionFrames && frame == static_cast<std::uint32_t>(*frameCount - 1)) {
            section = static_cast<std::uint32_t>(*frameCount / *sectionFrames + 1);
            frame = 0;
        } else frame %= static_cast<std::uint32_t>(*sectionFrames);
        const auto sectionBlock = in.read<std::int32_t>(desc + *sectionOffset + std::int64_t(section) * 8);
        const auto sectionData = in.read<std::int32_t>(desc + *sectionOffset + std::int64_t(section) * 8 + 4);
        if (!sectionBlock || !sectionData) return {};
        block = *sectionBlock;
        data = desc + *sectionData;
    }
    if (block != 0) return {}; // external .ani block
    if (*animOffset == 0 && *sectionFrames == 0) return {};

    const std::size_t boneCount = mdl.bones.size();
    std::vector<Local> locals(boneCount);
    for (std::size_t i = 0; i < boneCount; ++i) {
        const auto& bone = mdl.bones[i];
        locals[i].rotation = {bone.rotationQuat[0], bone.rotationQuat[1], bone.rotationQuat[2], bone.rotationQuat[3]};
        locals[i].position = {bone.position[0], bone.position[1], bone.position[2]};
    }

    std::int64_t entry = data;
    bool ended = false;
    for (int guard = 0; guard < 4096; ++guard) {
        const auto boneIndex = in.read<std::uint8_t>(entry);
        const auto flags = in.read<std::uint8_t>(entry + 1);
        const auto next = in.read<std::int16_t>(entry + 2);
        if (!boneIndex || !flags || !next) return {};
        if (*boneIndex == 255) { ended = true; break; }
        if (*boneIndex >= boneCount || (*flags & (kDelta | 0xC0))) return {};
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
                    if (*offset > 0) {
                        const auto value = animValue(in, base + *offset, frame);
                        if (!value) return {};
                        angles[axis] += *value * bone.rotationScale[axis];
                    }
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
                    if (*offset > 0) {
                        const auto value = animValue(in, base + *offset, frame);
                        if (!value) return {};
                        local.position[axis] += *value * bone.positionScale[axis];
                    }
                }
            }
        }
        if (*next == 0) { ended = true; break; }
        if (*next < 4) return {};
        entry += *next;
    }

    if (!ended) return {};
    return locals;
}

Quat interpolate(Quat a, Quat b, float fraction) {
    float dot = a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w;
    if (dot < 0) { b = {-b.x, -b.y, -b.z, -b.w}; dot = -dot; }
    float wa = 1 - fraction, wb = fraction;
    if (dot < 0.9995f) {
        const float angle = std::acos(std::clamp(dot, -1.0f, 1.0f));
        wa = std::sin((1 - fraction) * angle) / std::sin(angle);
        wb = std::sin(fraction * angle) / std::sin(angle);
    }
    return {a.x*wa+b.x*wb, a.y*wa+b.y*wb, a.z*wa+b.z*wb, a.w*wa+b.w*wb};
}
} // namespace

std::optional<std::vector<BoneMatrix>> sequenceSkinning(const mdlpp::MDL::MDL& mdl,
    std::span<const std::byte> bytes, int sequence, double frame) {
    if (!std::isfinite(frame) || sequence < 0 || static_cast<std::size_t>(sequence) >= mdl.sequences.size()) return {};
    const auto& seq = mdl.sequences[static_cast<std::size_t>(sequence)];
    if (seq.animIndices.empty() || seq.animIndices.front() < 0) return {};
    const Reader in(bytes);
    const auto animBase = in.read<std::int32_t>(kHeaderAnimIndex);
    if (!animBase) return {};
    const auto count = in.read<std::int32_t>(std::int64_t(*animBase) + std::int64_t(seq.animIndices.front()) * kAnimDescSize + 16);
    if (!count || *count <= 0) return {};
    frame = std::clamp(frame, 0.0, double(*count - 1));
    const auto first = static_cast<std::uint32_t>(std::floor(frame));
    auto sampled = sampleLocals(mdl, bytes, sequence, first);
    if (!sampled) return {};
    auto& locals = *sampled;
    const auto fraction = static_cast<float>(frame - first);
    if (fraction > 0) {
        const auto next = sampleLocals(mdl, bytes, sequence, first + 1);
        if (!next) return {};
        for (std::size_t i = 0; i < locals.size(); ++i) {
            locals[i].rotation = interpolate(locals[i].rotation, (*next)[i].rotation, fraction);
            for (int axis = 0; axis < 3; ++axis)
                locals[i].position[axis] += ((*next)[i].position[axis] - locals[i].position[axis]) * fraction;
        }
    }
    const auto boneCount = mdl.bones.size();
    std::vector<BoneMatrix> world(boneCount);
    std::vector<BoneMatrix> skinning(boneCount);
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
        if (bone.parent < -1 || bone.parent >= static_cast<std::int32_t>(i)) return {};
        world[i] = bone.parent < 0 ? local : multiply(world[static_cast<std::size_t>(bone.parent)], local);

        BoneMatrix poseToBone{};
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c) poseToBone[r * 4 + c] = bone.poseToBose[r][c];
        skinning[i] = multiply(world[i], poseToBone);
        for (const float v : skinning[i])
            if (!std::isfinite(v)) return {};
    }
    return skinning;
}

struct ModelAnimationData {
    struct Weight { std::array<std::uint8_t, 3> bones{}; std::array<float, 3> values{}; std::uint8_t count = 0; };
    mdlpp::MDL::MDL skeleton{};
    std::vector<std::byte> bytes;
    std::vector<ModelVertex> reference;
    std::vector<Weight> weights;
};

void retainModelAnimation(ModelData& model, const mdlpp::MDL::MDL& mdl, const mdlpp::VVD::VVD& vvd,
    std::span<const std::byte> bytes, std::span<const std::uint32_t> vertexSources) {
    if (bytes.empty() || mdl.sequences.empty()) return;
    auto data = std::make_shared<ModelAnimationData>();
    data->skeleton.bones = mdl.bones;
    data->skeleton.sequences.resize(mdl.sequences.size());
    for (std::size_t i = 0; i < mdl.sequences.size(); ++i)
        data->skeleton.sequences[i].animIndices = mdl.sequences[i].animIndices;
    data->bytes.assign(bytes.begin(), bytes.end());
    const Reader in(bytes);
    const auto animCount = in.read<std::int32_t>(kHeaderAnimCount).value_or(0);
    const auto animBase = in.read<std::int32_t>(kHeaderAnimIndex).value_or(0);
    bool any = false;
    for (std::size_t i = 0; i < mdl.sequences.size(); ++i) {
        const auto& source = mdl.sequences[i];
        ModelSequence sequence;
        sequence.bounds = {DVec3{source.boundingBoxMin[0], source.boundingBoxMin[1], source.boundingBoxMin[2]},
                           DVec3{source.boundingBoxMax[0], source.boundingBoxMax[1], source.boundingBoxMax[2]}};
        sequence.name = source.label.empty() ? "Sequence " + std::to_string(i) : source.label;
        const int animation = source.animIndices.empty() ? -1 : source.animIndices.front();
        const auto desc = std::int64_t(animBase) + std::int64_t(animation) * kAnimDescSize;
        sequence.frames = std::max(1, in.read<std::int32_t>(desc + 16).value_or(1));
        sequence.fps = in.read<float>(desc + 8).value_or(30);
        if (!(sequence.fps > 0) || !std::isfinite(sequence.fps)) sequence.fps = 30;
        if (mdl.bones.empty()) sequence.reason = "No skeleton";
        else if (animation < 0 || animation >= animCount) sequence.reason = "Animation data is not available in this model";
        else if (source.animIndices.size() != 1) sequence.reason = "Pose-parameter blending is not supported yet";
        else if (!source.autoLayers.empty()) sequence.reason = "Layered sequences are not supported yet";
        else if (in.read<std::int32_t>(desc + 12).value_or(0) & kAnimDescDelta) sequence.reason = "Delta animations are not supported yet";
        else if (in.read<std::int32_t>(desc + 52).value_or(0) != 0) sequence.reason = "External .ani animation data is not supported yet";
        else if (!sequenceSkinning(mdl, bytes, static_cast<int>(i), 0) ||
                 !sequenceSkinning(mdl, bytes, static_cast<int>(i), sequence.frames - 1))
            sequence.reason = "Animation data is unavailable or uses an unsupported encoding";
        else sequence.supported = true;
        any = any || sequence.supported;
        model.sequences.push_back(std::move(sequence));
    }
    if (!any || vertexSources.size() != model.vertices.size()) return;
    data->reference.reserve(vertexSources.size());
    data->weights.reserve(vertexSources.size());
    for (const auto source : vertexSources) {
        if (source >= vvd.vertices.size()) return;
        const auto& vertex = vvd.vertices[source];
        data->reference.push_back({{vertex.position[0], vertex.position[1], vertex.position[2]},
                                   {vertex.normal[0], vertex.normal[1], vertex.normal[2]}, {vertex.uv[0], vertex.uv[1]}});
        ModelAnimationData::Weight weight;
        const auto count = std::min<std::size_t>({3, vertex.boneWeight.bones.size(), vertex.boneWeight.weight.size()});
        weight.count = static_cast<std::uint8_t>(count);
        for (std::size_t i = 0; i < count; ++i) {
            weight.bones[i] = static_cast<std::uint8_t>(std::max<int>(0, vertex.boneWeight.bones[i]));
            weight.values[i] = vertex.boneWeight.weight[i];
        }
        if (!count) { weight.count = 1; weight.values[0] = 1; }
        data->weights.push_back(weight);
    }
    model.animation = std::move(data);
}

std::vector<ModelVertex> animateModel(const ModelData& model, int sequence, double frame) {
    if (!model.animation || sequence < 0 || static_cast<std::size_t>(sequence) >= model.sequences.size() ||
        !model.sequences[static_cast<std::size_t>(sequence)].supported) return {};
    const auto& data = *model.animation;
    const auto matrices = sequenceSkinning(data.skeleton, data.bytes, sequence, frame);
    if (!matrices) return {};
    auto result = data.reference;
    for (std::size_t i = 0; i < result.size(); ++i) {
        const auto& source = data.reference[i];
        const auto& weights = data.weights[i];
        Vec3 position{}, normal{};
        float total = 0;
        for (std::uint8_t j = 0; j < weights.count; ++j) {
            const auto bone = weights.bones[j];
            const auto weight = weights.values[j];
            if (bone >= matrices->size() || !(weight > 0) || !std::isfinite(weight)) continue;
            const auto& m = (*matrices)[bone];
            for (int r = 0; r < 3; ++r) {
                position[r] += weight * (m[r*4] * source.position.x + m[r*4+1] * source.position.y + m[r*4+2] * source.position.z + m[r*4+3]);
                normal[r] += weight * (m[r*4] * source.normal.x + m[r*4+1] * source.normal.y + m[r*4+2] * source.normal.z);
            }
            total += weight;
        }
        if (total > 0) {
            result[i].position = position / total;
            if (!std::isfinite(result[i].position.x) || !std::isfinite(result[i].position.y) || !std::isfinite(result[i].position.z)) return {};
            if (glm::length(normal) > 1e-6f) result[i].normal = glm::normalize(normal);
        }
    }
    return result;
}

std::size_t modelAnimationByteSize(const ModelData& model) {
    std::size_t bytes = 0;
    for (const auto& sequence : model.sequences) bytes += sizeof(sequence) + sequence.name.size() + sequence.reason.size();
    if (model.animation) {
        const auto& data = *model.animation;
        bytes += sizeof(data) + data.bytes.size() + data.reference.size() * sizeof(ModelVertex) +
                 data.weights.size() * sizeof(ModelAnimationData::Weight) + data.skeleton.bones.size() * sizeof(mdlpp::MDL::Bone);
        for (const auto& bone : data.skeleton.bones) bytes += bone.name.size() + bone.surfacePropName.size();
        for (const auto& sequence : data.skeleton.sequences)
            bytes += sizeof(sequence) + sequence.label.size() + sequence.activityName.size() + sequence.animIndices.size() * sizeof(std::int16_t);
    }
    return bytes;
}

std::vector<BoneMatrix> sequenceZeroSkinning(const mdlpp::MDL::MDL& mdl, std::span<const std::byte> bytes) {
    auto sampled = sequenceSkinning(mdl, bytes, 0, 0);
    if (!sampled || std::ranges::all_of(*sampled, nearIdentity)) return {};
    return std::move(*sampled);
}

} // namespace apex::assets
