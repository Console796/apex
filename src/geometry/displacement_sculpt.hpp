#pragma once

// Plan 0.8 D6 (0.8.4): the Displacement tool's Sculpt and Paint alpha strokes as pure operations.
// A DisplacementStroke holds a set of displacements with their live meshes; the tool feeds it
// brush centres (dabs) between press and release, previews the live values and commits the
// touched ones once. Vertices that sewn neighbours share, found by Sew's matching, move as one,
// and a T-edge vertex follows the coarser edge, so a stroke never opens a seam (plan §5 bite 6).

#include "geometry/displacement_ops.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace apex::geometry {

enum class SculptBrush { RaiseLower, Smooth, Flatten, SetHeight, Noise };
enum class BrushFalloff { Constant, Linear, Smooth };
// VertexNormal is the live surface normal (area-weighted, as drawn); FaceNormal is the base face's.
enum class SculptDirection { VertexNormal, FaceNormal, AxisX, AxisY, AxisZ };

struct SculptSettings {
    SculptBrush brush = SculptBrush::RaiseLower;
    BrushFalloff falloff = BrushFalloff::Smooth;
    SculptDirection direction = SculptDirection::FaceNormal;
    double radius = 64.0;
    // Raise/Lower: units moved per dab at the centre (negative lowers). Noise: the largest move.
    double amount = 2.0;
    // Smooth, Flatten and Set height: the fraction of the way to the target per dab, 0..1.
    double strength = 0.25;
    // Set height: the distance from the base face along the direction (Vertex normal reads as
    // Face normal here, since a height needs a fixed direction).
    double height = 0.0;
    // Noise: a stroke with the same seed, start and dabs moves the same vertices the same way.
    std::uint32_t seed = 1;
};

struct PaintSettings {
    BrushFalloff falloff = BrushFalloff::Smooth;
    double radius = 64.0;
    double strength = 0.5; // Fraction of the way to alpha per dab, 0..1.
    double alpha = 255.0;  // Target, 0..255.
};

// A vertex's weight at `distance` from the brush centre: 1 at the centre, 0 at and past radius.
double brushWeight(BrushFalloff falloff, double distance, double radius);

// Stores vertex i's displacement vector the way Hammer's painting does: the whole of
// normal * distance becomes `vector`, kept as a unit normal and a non-negative distance (normal
// 0 0 0 at distance 0); the offset (subdivision's) stays. test1's rows agree: every distance is
// non-negative and every zero distance has a zero normal. D2's rule (vector + offset) still holds.
void setDisplacementVector(Displacement& disp, std::size_t i, DVec3 vector);

class DisplacementStroke {
public:
    // Fails when a patch does not mesh. Every patch starts editable.
    static Result<DisplacementStroke> create(std::vector<DisplacementPatch> patches);

    // Which patches the brush acts on (size() entries). A vertex shared with an editable patch
    // still moves on every patch that holds it, so selecting one side of a seam cannot crack it.
    void setEditable(const std::vector<bool>& editable);

    struct Hit {
        std::size_t patch = 0;
        DVec3 point{0.0}, normal{0.0};
        double t = 0.0; // Along the ray's own parametrisation.
    };
    // The nearest forward hit on the live meshes, either side facing.
    std::optional<Hit> raycast(const Ray& ray) const;

    // Starts a stroke: clears the touched set, the flatten plane and the dab count.
    void begin();
    // One dab of the current stroke centred on `centre` (normally a raycast hit).
    void sculpt(const SculptSettings& settings, DVec3 centre);
    // Paints alpha; a painted vertex starts from its alpha clamped to 0..255, so out-of-range
    // values (D1) change only where painted over.
    void paint(const PaintSettings& settings, DVec3 centre);
    // Recomputes the triangle tags of the patches the stroke moved (the slope rule of Create and Sew).
    Result<void> finish();
    // Puts every touched patch back as it was at begin().
    void revert();

    std::size_t size() const { return patches_.size(); }
    const DisplacementPatch& patch(std::size_t p) const { return patches_[p].value; }
    const std::vector<DVec3>& positions(std::size_t p) const { return patches_[p].positions; }
    // Patches the current stroke changed, ascending.
    std::vector<std::size_t> touched() const;
    std::size_t dabs() const { return dabs_; }
    // Replaces patch p's displacement with one of the same power (a commit read back), keeping
    // the neighbour matching. Fails for another power or a value that does not mesh.
    Result<void> reset(std::size_t p, Displacement disp);

private:
    struct Patch {
        DisplacementPatch value;
        std::vector<DVec3> flat, positions, normals;
        DVec3 baseNormal{0.0};
        AABB bounds;
        bool normalsDirty = true, editable = true;
    };
    struct TEdge {
        std::uint32_t group = 0; // The finer side's vertex group...
        std::size_t q = 0, w = 0, w2 = 0; // ...lies on q's edge between w and w2 at s.
        double s = 0.0;
    };
    struct Affected {
        std::uint32_t group = 0;
        double weight = 0.0;
    };

    std::size_t global(std::size_t p, std::size_t v) const { return offsets_[p] + v; }
    std::span<const std::pair<std::uint32_t, std::uint32_t>> members(std::uint32_t group) const {
        return {members_.data() + groupStart_[group], members_.data() + groupStart_[group + 1]};
    }
    void touch(std::size_t p);
    void refreshNormals(std::size_t p);
    void refreshBounds(std::size_t p);
    std::vector<Affected> affected(DVec3 centre, double radius, BrushFalloff falloff);
    DVec3 direction(SculptDirection direction, std::uint32_t group);
    void move(std::uint32_t group, DVec3 delta);

    std::vector<Patch> patches_;
    std::map<int, std::vector<std::array<std::uint32_t, 3>>> triangles_; // By power.
    std::vector<std::size_t> offsets_;
    std::vector<std::uint32_t> groupOf_;                         // Global vertex -> group.
    std::vector<std::size_t> groupStart_;                         // CSR into members_.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> members_; // (patch, vertex).
    std::vector<TEdge> tEdges_;
    std::vector<std::uint64_t> stamp_; // Per group: the dab that last saw it.
    std::uint64_t clock_ = 0;

    std::map<std::size_t, Displacement> originals_; // Touched patches at begin().
    std::set<std::size_t> moved_;                    // Touched patches whose geometry changed.
    std::optional<std::pair<DVec3, DVec3>> plane_;   // Flatten: point and unit normal.
    std::optional<DVec3> start_;                     // The stroke's first centre (noise seed).
    std::size_t dabs_ = 0;
};

} // namespace apex::geometry
