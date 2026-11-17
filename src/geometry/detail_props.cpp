#include "geometry/detail_props.hpp"

#include "core/kv1.hpp"
#include "core/strings.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numbers>

namespace apex::geometry {
namespace {

Error invalid(std::string message) { return {ErrorCode::InvalidArgument, std::move(message)}; }

bool hasSubkeys(const kv1::Node& node) { return node.kind() == kv1::NodeKind::Block && !node.children().empty(); }

// KeyValues' getters: a missing key gives the default; a present one reads as atof / atoi would.
std::optional<std::string> keyText(const kv1::Node& block, std::string_view key) {
    const auto* node = block.find(key);
    if (!node || node->kind() != kv1::NodeKind::Value) return std::nullopt;
    auto text = node->decodedValue();
    if (!text) return std::nullopt;
    return std::move(*text);
}
float keyFloat(const kv1::Node& block, std::string_view key, float fallback) {
    const auto text = keyText(block, key);
    return text ? static_cast<float>(std::strtod(text->c_str(), nullptr)) : fallback;
}
int keyInt(const kv1::Node& block, std::string_view key, int fallback) {
    const auto text = keyText(block, key);
    return text ? static_cast<int>(std::strtod(text->c_str(), nullptr)) : fallback;
}
// sscanf("%f %f ..."): fills a prefix of the outputs and leaves the rest alone.
int scanFloats(const std::string& text, std::span<float*> out) {
    const char* at = text.c_str();
    int count = 0;
    for (auto* target : out) {
        char* end = nullptr;
        const float value = std::strtof(at, &end);
        if (end == at) break;
        *target = value;
        at = end;
        ++count;
    }
    return count;
}

Result<void> parseGroup(DetailType& type, const kv1::Node& block) {
    const float alpha = keyFloat(block, "alpha", 1.0f);
    // Insert after the last group more transparent than this one.
    auto i = static_cast<int>(type.groups.size());
    while (--i >= 0)
        if (alpha > type.groups[static_cast<std::size_t>(i)].alpha) break;
    auto& group = *type.groups.insert(type.groups.begin() + (i + 1), DetailGroup{alpha, {}});

    float total = 0.0f;
    for (const auto& entry : block.children()) {
        if (!hasSubkeys(entry)) continue;
        DetailModel model;
        model.name = std::string(entry.key());
        if (auto path = keyText(entry, "model")) {
            model.type = DetailPropType::Model;
            model.model = std::move(*path);
        } else if (const auto sprite = keyText(entry, "sprite")) {
            model.type = DetailPropType::Sprite;
            if (const auto shape = keyText(entry, "sprite_shape")) {
                if (ciEquals(*shape, "cross")) model.type = DetailPropType::ShapeCross;
                else if (ciEquals(*shape, "tri")) model.type = DetailPropType::ShapeTri;
            }
            float x = 0, y = 0, width = 64, height = 64, textureSize = 512;
            std::array<float*, 5> fields{&x, &y, &width, &height, &textureSize};
            if (scanFloats(*sprite, fields) != 5 || textureSize == 0)
                return std::unexpected(invalid("detail.vbsp: invalid \"sprite\" in " + model.name));
            model.tex[0] = {(x + 0.5f) / textureSize, (y + 0.5f) / textureSize};
            model.tex[1] = {(x + width - 0.5f) / textureSize, (y + height - 0.5f) / textureSize};
            if (const auto size = keyText(entry, "spritesize")) {
                std::array<float*, 4> sizeFields{&x, &y, &width, &height};
                scanFloats(*size, sizeFields);
                const float ox = width * x, oy = height * y;
                model.pos[0] = {-ox, height - oy};
                model.pos[1] = {width - ox, -oy};
            }
            model.randomScaleStdDev = keyFloat(entry, "spriterandomscale", 0.0f);
            model.sway = static_cast<std::uint8_t>(255.0 * std::clamp(double(keyFloat(entry, "sway", 0.0f)), 0.0, 1.0));
            model.shapeAngle = static_cast<std::uint8_t>(keyInt(entry, "shape_angle", 0));
            model.shapeSize =
                static_cast<std::uint8_t>(255.0 * std::clamp(double(keyFloat(entry, "shape_size", 0.0f)), 0.0, 1.0));
        }
        model.amount = keyFloat(entry, "amount", 1.0f) + total;
        total = model.amount;
        model.upright = keyInt(entry, "upright", 0) != 0;
        // These keep props off steep surfaces.
        const float minAngle = keyFloat(entry, "minAngle", 180.0f), maxAngle = keyFloat(entry, "maxAngle", 180.0f);
        model.minCosAngle = static_cast<float>(std::cos(minAngle * std::numbers::pi / 180.0f));
        model.maxCosAngle = static_cast<float>(std::cos(maxAngle * std::numbers::pi / 180.0f));
        model.orientation = keyInt(entry, "detailOrientation", 0);
        model.minCosAngle = std::max(model.minCosAngle, model.maxCosAngle);
        group.models.push_back(std::move(model));
    }
    if (total > 1.0f)
        for (auto& model : group.models) model.amount /= total;
    return {};
}

// MSVC's rand(), which Windows vbsp.exe draws from after srand(hammerfaceid).
struct CRand {
    std::uint32_t state;
    int next() {
        state = state * 214013u + 2531011u;
        return static_cast<int>((state >> 16) & 0x7fff);
    }
    float unit() { return static_cast<float>(next()) / 32767.0f; }
};

// vstdlib's CUniformRandomStream (Numerical Recipes ran1) and the Box-Muller CGaussianRandomStream on top.
class GaussianStream {
public:
    explicit GaussianStream(int seed) : idum_(seed < 0 ? seed : -seed) {}
    float next(float mean, float stdDev) {
        if (haveValue_) {
            haveValue_ = false;
            return stdDev * cached_ + mean;
        }
        float v1, v2, rsq;
        do {
            v1 = 2.0f * uniform() - 1.0f;
            v2 = 2.0f * uniform() - 1.0f;
            rsq = v1 * v1 + v2 * v2;
        } while (rsq > 1.0f || rsq == 0.0f);
        const float fac = std::sqrt(-2.0f * std::log(rsq) / rsq);
        cached_ = v1 * fac;
        haveValue_ = true;
        return stdDev * (v2 * fac) + mean;
    }

private:
    static constexpr int kA = 16807, kM = 2147483647, kQ = 127773, kR = 2836, kTab = 32;
    static constexpr int kDiv = 1 + (kM - 1) / kTab;
    int generate() {
        int k;
        if (idum_ <= 0 || !iy_) {
            idum_ = -idum_ < 1 ? 1 : -idum_;
            for (int j = kTab + 7; j >= 0; --j) {
                k = idum_ / kQ;
                idum_ = kA * (idum_ - k * kQ) - kR * k;
                if (idum_ < 0) idum_ += kM;
                if (j < kTab) iv_[static_cast<std::size_t>(j)] = idum_;
            }
            iy_ = iv_[0];
        }
        k = idum_ / kQ;
        idum_ = kA * (idum_ - k * kQ) - kR * k;
        if (idum_ < 0) idum_ += kM;
        const auto j = static_cast<std::size_t>((iy_ / kDiv) & (kTab - 1));
        iy_ = iv_[j];
        iv_[j] = idum_;
        return iy_;
    }
    float uniform() {
        float value = static_cast<float>((1.0 / kM) * generate());
        if (value > 1.0 - 1.2e-7) value = static_cast<float>(1.0 - 1.2e-7);
        return value;
    }
    int idum_, iy_ = 0;
    std::array<int, kTab> iv_{};
    float cached_ = 0.0f;
    bool haveValue_ = false;
};

Vec3 normalized(Vec3 v) {
    const float length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return length != 0.0f ? v / length : v;
}

struct SurfaceVert {
    Vec3 flat, position;
    float alpha;
};

// CCoreDispInfo after DispMapToCoreDispInfo and GenerateDispSurf, in VBSP's float arithmetic.
class CoreDisp {
public:
    CoreDisp(std::span<const DVec3> corners, const Displacement& disp) : width_(disp.size()) {
        // Source winding is clockwise from outside, rotated to start at the corner nearest startPosition.
        std::array<Vec3, 4> source;
        for (std::size_t k = 0; k < 4; ++k) source[k] = Vec3(corners[3 - k]);
        const Vec3 start(disp.startPosition);
        std::size_t first = 0;
        float best = 999999999.0f;
        for (std::size_t k = 0; k < 4; ++k) {
            const Vec3 d = start - source[k];
            if (const float distanceSq = d.x * d.x + d.y * d.y + d.z * d.z; distanceSq < best) {
                best = distanceSq;
                first = k;
            }
        }
        for (std::size_t k = 0; k < 4; ++k) p_[k] = source[(first + k) % 4];

        const float step = 1.0f / static_cast<float>(width_ - 1);
        const Vec3 edge0 = (p_[1] - p_[0]) * step, edge1 = (p_[2] - p_[3]) * step;
        verts_.reserve(disp.vertexCount());
        for (int i = 0; i < width_; ++i) {
            const Vec3 end0 = edge0 * static_cast<float>(i) + p_[0], end1 = edge1 * static_cast<float>(i) + p_[3];
            const Vec3 segment = (end1 - end0) * step;
            for (int j = 0; j < width_; ++j) {
                const auto index = static_cast<std::size_t>(i * width_ + j);
                // DispMapToCoreDispInfo folds the offset into a field vector and distance.
                const Vec3 field = Vec3(disp.normals[index]) * static_cast<float>(disp.distances[index]) +
                                   Vec3(disp.offsets[index]);
                const float distance = std::sqrt(field.x * field.x + field.y * field.y + field.z * field.z);
                SurfaceVert vert;
                vert.flat = end0 + segment * static_cast<float>(j);
                vert.position = vert.flat + normalized(field) * distance;
                vert.alpha = static_cast<float>(disp.alphas[index]);
                verts_.push_back(vert);
            }
        }
    }

    // ComputeDisplacementFaceArea: the base face as two triangles.
    float baseArea() const {
        float area = 0.0f;
        for (std::size_t i = 1; i <= 2; ++i) {
            const Vec3 e1 = p_[i] - p_[0], e2 = p_[i + 1] - p_[0];
            const Vec3 c = glm::cross(e1, e2);
            area += 0.5f * std::sqrt(c.x * c.x + c.y * c.y + c.z * c.z);
        }
        return area;
    }

    // DispUVToSurf. Returns false where VBSP leaves its outputs unset (both triangles reject the point).
    bool surface(float u, float v, Vec3& point, Vec3& normal, float& alpha) const {
        const Vec3 a = p_[0] + (p_[1] - p_[0]) * v, b = p_[3] + (p_[2] - p_[3]) * v;
        const Vec3 intersect = a + (b - a) * u;
        const float flU = u * (static_cast<float>(width_) - 1.000001f);
        const float flV = v * (static_cast<float>(width_) - 1.000001f);
        const int snapU = static_cast<int>(flU), snapV = static_cast<int>(flV);
        const int nextU = std::min(snapU + 1, width_ - 1), nextV = std::min(snapV + 1, width_ - 1);
        const float fracU = flU - static_cast<float>(snapU), fracV = flV - static_cast<float>(snapV);
        const auto at = [&](int row, int column) { return row * width_ + column; };
        // Each triangle: its vertex indices and the two edges whose cross is its normal.
        struct Tri {
            std::array<int, 3> index;
            int edgeA[2], edgeB[2];
        };
        Tri first, second;
        if ((snapV * width_ + snapU) % 2 == 1) { // Top left to bottom right.
            const Tri one{{at(nextV, snapU), at(nextV, nextU), at(snapV, nextU)}, {0, 1}, {2, 1}};
            const Tri two{{at(snapV, snapU), at(nextV, snapU), at(snapV, nextU)}, {2, 0}, {1, 0}};
            const bool firstIsOne = fracU + fracV >= 1.0f + 0.00001f;
            first = firstIsOne ? one : two;
            second = firstIsOne ? two : one;
        } else { // Bottom left to top right.
            const Tri one{{at(snapV, snapU), at(nextV, snapU), at(nextV, nextU)}, {0, 1}, {2, 1}};
            const Tri two{{at(snapV, snapU), at(nextV, nextU), at(snapV, nextU)}, {1, 2}, {0, 2}};
            const bool firstIsOne = fracU < fracV;
            first = firstIsOne ? one : two;
            second = firstIsOne ? two : one;
        }
        for (const auto& tri : {first, second}) {
            std::array<float, 3> c;
            if (!barycentric(tri.index, intersect, c)) continue;
            const auto& v0 = verts_[static_cast<std::size_t>(tri.index[0])];
            const auto& v1 = verts_[static_cast<std::size_t>(tri.index[1])];
            const auto& v2 = verts_[static_cast<std::size_t>(tri.index[2])];
            point = v0.position * c[0] + v1.position * c[1] + v2.position * c[2];
            alpha = v0.alpha * c[0] + v1.alpha * c[1] + v2.alpha * c[2];
            const auto pos = [&](int k) { return verts_[static_cast<std::size_t>(tri.index[static_cast<std::size_t>(k)])].position; };
            normal = normalized(glm::cross(pos(tri.edgeA[0]) - pos(tri.edgeA[1]), pos(tri.edgeB[0]) - pos(tri.edgeB[1])));
            return true;
        }
        return false;
    }

private:
    // CalcBarycentricCooefs on the flat triangle: areas, accepted when they sum to 1 within 1e-3.
    bool barycentric(const std::array<int, 3>& index, Vec3 pt, std::array<float, 3>& c) const {
        const Vec3 v0 = verts_[static_cast<std::size_t>(index[0])].flat;
        const Vec3 v1 = verts_[static_cast<std::size_t>(index[1])].flat;
        const Vec3 v2 = verts_[static_cast<std::size_t>(index[2])].flat;
        const auto area = [](Vec3 a, Vec3 b) {
            const Vec3 cross = glm::cross(a, b);
            return std::sqrt(cross.x * cross.x + cross.y * cross.y + cross.z * cross.z) * 0.5f;
        };
        const float total = area(v1 - v0, v2 - v0);
        const float inverse = total != 0.0f ? 1.0f / total : 0.0f;
        c[0] = area(v1 - pt, v2 - pt) * inverse;
        c[1] = area(v2 - pt, v0 - pt) * inverse;
        c[2] = area(v0 - pt, v1 - pt) * inverse;
        return std::fabs(1.0f - (c[0] + c[1] + c[2])) < 1e-3;
    }

    int width_;
    std::array<Vec3, 4> p_;
    std::vector<SurfaceVert> verts_;
};

int selectGroup(const DetailType& type, float alpha, CRand& rand) {
    const auto count = static_cast<int>(type.groups.size());
    int start = 0;
    for (; start < count - 1; ++start)
        if (alpha < type.groups[static_cast<std::size_t>(start + 1)].alpha) break;
    int end = start + 1;
    if (end >= count) --end;
    if (start == end) return start;
    const float startAlpha = type.groups[static_cast<std::size_t>(start)].alpha;
    const float range = type.groups[static_cast<std::size_t>(end)].alpha - startAlpha;
    const float dist = range != 0.0f ? (alpha - startAlpha) / range : 0.0f;
    return rand.unit() > dist ? start : end;
}

int selectModel(const DetailGroup& group, CRand& rand) {
    const float r = rand.unit();
    for (std::size_t i = 0; i < group.models.size(); ++i)
        if (r <= group.models[i].amount) return static_cast<int>(i);
    return -1;
}

constexpr float kRadToDeg = 180.0f / std::numbers::pi_v<float>;

// MatrixToAngles on a matrix whose columns are forward, left and up.
Vec3 anglesOf(Vec3 forward, Vec3 left, Vec3 up) {
    const float xyDist = std::sqrt(forward.x * forward.x + forward.y * forward.y);
    if (xyDist > 0.001f)
        return {std::atan2(-forward.z, xyDist) * kRadToDeg, std::atan2(forward.y, forward.x) * kRadToDeg,
                std::atan2(left.z, up.z) * kRadToDeg};
    return {std::atan2(-forward.z, xyDist) * kRadToDeg, std::atan2(-left.x, left.y) * kRadToDeg, 0.0f};
}

// PlaceDetail, minus writing the lump. Returns false where VBSP emits nothing.
bool placeOne(const DetailModel& model, Vec3 point, Vec3 normal, CRand& rand, GaussianStream& gaussian,
              DetailProp& prop) {
    const float cosAngle = normal.z;
    if (cosAngle < model.maxCosAngle) return false;
    if (cosAngle < model.minCosAngle) {
        const float probability = (cosAngle - model.maxCosAngle) / (model.minCosAngle - model.maxCosAngle);
        if (rand.unit() > probability) return false;
    }
    if (model.upright) {
        prop.angles = {0.0f, 360.0f * static_cast<float>(rand.next()) / 32767.0f, 0.0f};
    } else {
        const Vec3 z = normalized(normal);
        Vec3 x{1.0f, 0.0f, 0.0f};
        if (std::fabs(glm::dot(x, z)) - 1.0 > -1e-3) x = {0.0f, 1.0f, 0.0f};
        const Vec3 y = normalized(glm::cross(z, x));
        x = normalized(glm::cross(y, z));
        const float rotation = 360.0f * static_cast<float>(rand.next()) / 32767.0f / kRadToDeg;
        const float c = std::cos(rotation), s = std::sin(rotation);
        // The basis times a rotation about its own z.
        prop.angles = anglesOf(x * c + y * s, y * c - x * s, z);
    }
    prop.origin = point;
    prop.scale = 1.0f;
    if (model.type != DetailPropType::Model && model.randomScaleStdDev != 0.0f)
        prop.scale = std::fabs(gaussian.next(1.0f, model.randomScaleStdDev));
    return true;
}

} // namespace

const DetailType* DetailVbsp::find(std::string_view name) const {
    for (const auto& type : types)
        if (type.name == name) return &type;
    return nullptr;
}

Result<DetailVbsp> parseDetailVbsp(std::string_view text) {
    auto document = kv1::parse(std::string(text), kv1::EscapeMode::Legacy);
    if (!document) return std::unexpected(document.error());
    DetailVbsp result;
    const auto children = document->root().children();
    const auto root = std::ranges::find_if(children, hasSubkeys);
    if (root == children.end()) return result;
    for (const auto& block : root->children()) {
        if (!hasSubkeys(block)) continue;
        DetailType type;
        type.name = std::string(block.key());
        type.density = keyFloat(block, "density", 0.0f);
        for (const auto& group : block.children())
            if (hasSubkeys(group))
                if (auto parsed = parseGroup(type, group); !parsed) return std::unexpected(parsed.error());
        result.types.push_back(std::move(type));
    }
    return result;
}

Result<std::vector<DetailProp>> placeDetailProps(std::span<const DVec3> corners, const Displacement& disp,
                                                 const DetailType& type, int seed) {
    if (const auto problem = disp.problem()) return std::unexpected(invalid(*problem));
    if (corners.size() != 4) return std::unexpected(invalid("a displacement needs a four-sided base face"));
    std::vector<DetailProp> result;
    if (type.groups.empty()) return result; // VBSP would index past an empty group list.
    const CoreDisp surface(corners, disp);
    CRand rand{static_cast<std::uint32_t>(seed)};
    GaussianStream gaussian(seed);
    // float * float, widened to double for the 1e-6, then truncated.
    const int samples = static_cast<int>(static_cast<double>(surface.baseArea() * type.density) * 0.000001);
    for (int i = 0; i < samples; ++i) {
        const float u = rand.unit(), v = rand.unit();
        Vec3 point{0.0f}, normal{0.0f, 0.0f, 1.0f};
        float alpha = 0.0f;
        surface.surface(u, v, point, normal, alpha);
        alpha /= 255.0f;
        const int group = selectGroup(type, alpha, rand);
        const auto& models = type.groups[static_cast<std::size_t>(group)];
        const int model = selectModel(models, rand);
        if (model < 0) continue;
        DetailProp prop;
        prop.group = static_cast<std::uint16_t>(group);
        prop.model = static_cast<std::uint16_t>(model);
        if (placeOne(models.models[static_cast<std::size_t>(model)], point, normal, rand, gaussian, prop))
            result.push_back(prop);
    }
    return result;
}

} // namespace apex::geometry
