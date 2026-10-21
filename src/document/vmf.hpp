#pragma once

#include "core/kv1.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace apex::vmf {

using Vec3 = std::array<double, 3>;

struct PlanePoints {
    std::array<Vec3, 3> points{};
};

struct UvAxis {
    Vec3 axis{};
    double shift = 0.0;
    double scale = 1.0;
};

// A scalar view retains the KV1 node's raw spelling. Setters dirty only this value, allowing
// the KV1 writer to copy the rest of the containing VMF verbatim.
class Value {
public:
    Value() = default;
    explicit Value(kv1::Node* node) : node_(node) {}

    explicit operator bool() const { return node_ != nullptr; }
    std::string_view key() const;
    std::string_view raw() const;
    std::size_t line() const;
    Result<std::string> text() const;
    Result<int> asInt() const;
    Result<double> asDouble() const;
    Result<Vec3> asVec3() const;
    Result<PlanePoints> asPlane() const;
    Result<UvAxis> asUvAxis() const;

    Result<void> set(std::string value);
    Result<void> set(int value);
    Result<void> set(double value);
    Result<void> set(Vec3 value);
    Result<void> set(PlanePoints value);
    Result<void> set(UvAxis value);
    // Positions, origins and angles need round-trip precision; colors keep set(Vec3).
    Result<void> setGeometry(Vec3 value);
    // Only the axis components use geometry precision; shift/scale keep six digits.
    Result<void> setGeometry(UvAxis value);

private:
    kv1::Node* node_ = nullptr;
};

struct Block {
    std::string_view name() const;
    std::size_t line() const;
    std::span<Value> keyValues() { return values; }
    std::span<const Value> keyValues() const { return values; }
    Value* find(std::string_view key);
    const Value* find(std::string_view key) const;
    std::vector<Value*> findAll(std::string_view key);
    std::vector<const Value*> findAll(std::string_view key) const;

    kv1::Node* node = nullptr;
    std::vector<Value> values;
};

struct OpaqueBlock : Block {};

struct VersionInfo : Block {
    Value* editorExtension() { return find("editorextension"); }
    const Value* editorExtension() const { return find("editorextension"); }
};

struct Visgroup : Block {
    std::vector<Visgroup> children;
    std::vector<OpaqueBlock> unknownBlocks;
};

struct Visgroups : Block {
    std::vector<Visgroup> children;
    std::vector<OpaqueBlock> unknownBlocks;
};

struct View : Block {};

struct Views : Block {
    std::vector<View> children;
    std::vector<OpaqueBlock> unknownBlocks;
};

struct ViewSettings : Block {
    std::optional<Views> views;
    std::vector<OpaqueBlock> unknownBlocks;

    Value* instanceVisibility() { return find("nInstanceVisibility"); }
    const Value* instanceVisibility() const { return find("nInstanceVisibility"); }
};

struct PointData : Block {
    // point values have the VMF form "index x y z". They remain ordered and duplicated.
    Result<std::vector<Vec3>> points() const;
};

struct VerticesPlus : Block {
    Result<std::vector<Vec3>> points() const;
};

struct Side : Block {
    std::optional<PointData> pointData;
    std::optional<VerticesPlus> verticesPlus;
    std::optional<OpaqueBlock> dispInfo;
    std::vector<OpaqueBlock> unknownBlocks;

    Value* id() { return find("id"); }
    const Value* id() const { return find("id"); }
    Value* plane() { return find("plane"); }
    const Value* plane() const { return find("plane"); }
    Value* material() { return find("material"); }
    const Value* material() const { return find("material"); }
    Value* uaxis() { return find("uaxis"); }
    const Value* uaxis() const { return find("uaxis"); }
    Value* vaxis() { return find("vaxis"); }
    const Value* vaxis() const { return find("vaxis"); }
    Value* rotation() { return find("rotation"); }
    const Value* rotation() const { return find("rotation"); }
    Value* lightmapScale() { return find("lightmapscale"); }
    const Value* lightmapScale() const { return find("lightmapscale"); }
    Value* smoothingGroups() { return find("smoothing_groups"); }
    const Value* smoothingGroups() const { return find("smoothing_groups"); }
};

struct Solid : Block {
    std::vector<Side> sides;
    std::optional<OpaqueBlock> editor;
    std::vector<OpaqueBlock> unknownBlocks;

    Value* id() { return find("id"); }
    const Value* id() const { return find("id"); }
};

struct Group : Block {
    std::vector<Solid> solids;
    std::vector<Group> groups;
    std::optional<OpaqueBlock> editor;
    std::vector<OpaqueBlock> unknownBlocks;
};

struct Entity;

struct Hidden : Block {
    std::vector<Solid> solids;
    std::vector<Group> groups;
    std::vector<Hidden> hidden;
    std::vector<Entity> entities;
    std::vector<OpaqueBlock> unknownBlocks;
};

struct Entity : Block {
    std::vector<Solid> solids;
    std::vector<Group> groups;
    std::vector<Hidden> hidden;
    std::optional<OpaqueBlock> connections;
    std::optional<OpaqueBlock> editor;
    std::vector<OpaqueBlock> unknownBlocks;

    Value* id() { return find("id"); }
    const Value* id() const { return find("id"); }
};

struct World : Block {
    std::vector<Solid> solids;
    std::vector<Group> groups;
    std::vector<Hidden> hidden;
    std::optional<OpaqueBlock> editor;
    std::vector<OpaqueBlock> unknownBlocks;

    Value* id() { return find("id"); }
    const Value* id() const { return find("id"); }
};

struct Camera : Block {};
struct Cordon : Block {};

struct Cameras : Block {
    std::vector<Camera> cameras;
    std::vector<OpaqueBlock> unknownBlocks;
};

struct Cordons : Block {
    std::vector<Cordon> cordons;
    std::vector<OpaqueBlock> unknownBlocks;
};

enum class ProblemCode { DuplicateObjectId, DuplicateSideId, InvalidObjectId, InvalidSideId };

struct Problem {
    ProblemCode code;
    int id = 0;
    std::size_t line = 0;
    std::string message;
};

struct IdOccurrence {
    int id = 0;
    std::size_t line = 0;
    std::string_view kind;
};

class IdSpace {
public:
    std::span<const IdOccurrence> entries() const { return entries_; }
    std::size_t count(int id) const;
    bool contains(int id) const { return count(id) != 0; }
    std::optional<int> maximum() const;
    void record(IdOccurrence occurrence) { entries_.push_back(occurrence); }

private:
    std::vector<IdOccurrence> entries_;
};

struct ParseOptions {
    kv1::EscapeMode escapeMode = kv1::EscapeMode::Strata;
};

class VmfFile {
public:
    VmfFile(VmfFile&&) noexcept = default;
    VmfFile& operator=(VmfFile&&) noexcept = default;
    VmfFile(const VmfFile&) = delete;
    VmfFile& operator=(const VmfFile&) = delete;

    std::optional<VersionInfo> versionInfo;
    std::optional<Visgroups> visgroups;
    std::optional<ViewSettings> viewSettings;
    std::optional<World> world;
    std::vector<Entity> entities;
    std::vector<Group> groups;
    std::vector<Hidden> hidden;
    std::optional<Cameras> cameras;
    std::optional<Cordons> cordons;
    std::vector<OpaqueBlock> unknownBlocks;
    IdSpace objectIds; // Solids and entities share this VMF object-id space.
    IdSpace sideIds;
    std::vector<Problem> problems;

    std::string_view source() const { return document_.source(); }
    std::string_view lineEnding() const { return document_.lineEnding(); }
    const kv1::Document& kvDocument() const { return document_; }

private:
    friend Result<VmfFile> parse(std::string_view, ParseOptions);
    friend std::string write(const VmfFile&);
    explicit VmfFile(kv1::Document document) : document_(std::move(document)) {}
    kv1::Document document_;
};

Result<VmfFile> parse(std::string_view bytes, ParseOptions options = {});
std::string write(const VmfFile& file);

} // namespace apex::vmf
