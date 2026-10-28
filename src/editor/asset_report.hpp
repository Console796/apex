#pragma once

#include "editor/connection_validation.hpp"

#include <set>

namespace apex::editor {

enum class AssetKind { Material, Model, Sound, Particle, Instance };
struct AssetReference {
    document::NodeId entity = 0, solid = 0;
    std::optional<document::SideId> side;
    std::string key;
    bool operator==(const AssetReference&) const = default;
};
struct MissingAsset {
    AssetKind kind;
    std::string path, normalizedPath;
    std::vector<AssetReference> references;
    std::optional<std::string> addon;
};
using AssetExists = std::function<bool(AssetKind, std::string_view)>;
using AssetProvider = std::function<std::optional<std::string>(AssetKind, std::string_view)>;
// Conventional model/studio, sound, effect_name and func_instance file keys are
// references even if the FGD calls them strings. Other keys use FGD types, falling
// back to material/texture without metadata. Untyped message is not sound (0.4 D11).
std::optional<AssetKind> entityAssetKind(const kv1::Node& entity, std::string_view key,
                                      const assets::EntityClassTable* classes = nullptr);
struct ConnectionProblem {
    document::NodeId entity;
    std::size_t index;
    std::string output;
    std::vector<ConnectionIssue> issues;
};
struct UnknownClass { document::NodeId entity; std::string classname; };
struct Problems {
    std::vector<MissingAsset> missingAssets;
    std::vector<ConnectionProblem> invalidConnections;
    std::vector<UnknownClass> unknownClasses;
};
// On-demand snapshot, never VMF state. Paths passed to callbacks are lower-case with
// forward slashes; display paths retain the first reference's spelling.
class AssetReport {
public:
    AssetReport(const document::Document& doc, const AssetExists& exists,
                const AssetProvider& unmountedAddon = {}, const AssetProvider& mountedProvider = {},
                const assets::EntityClassTable* classes = nullptr);
    const std::vector<MissingAsset>& missing() const { return missing_; } // kind, then normalized path
    const std::set<std::string>& addonsUsed() const { return addons_; }
    Problems problems(const document::Document& doc, const ConnectionValidator& validator,
                      const std::function<bool(std::string_view)>& classKnown) const;
private:
    std::vector<MissingAsset> missing_;
    std::set<std::string> addons_;
};

} // namespace apex::editor
