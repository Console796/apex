#pragma once

#include "editor/clipboard.hpp"
#include "editor/placement_item.hpp"
#include "editor/transform.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace apex::editor {

class EditorSession;

// Prefabs (0.4 D12): plain VMF files in Strata Hammer's `hammer/prefabs` folder, so the two
// editors share them. Creating one writes the selection through the clipboard's fragment
// writer, wrapped as a full map (`versioninfo`, `visgroups`, a `world` holding the solids and
// groups, then the entities) that Hammer opens. Placing one goes through the clipboard's paste
// path, so a placement is one undo step with fresh IDs and kept groups, visgroups and
// `connections`. This file has no Qt and no asset-layer types: the UI passes plain paths.

// The default folder, `<gameDirectory>/../hammer/prefabs`, matching where the addon list lives.
// A non-empty `userFolder` (Preferences) replaces it.
std::filesystem::path prefabFolder(const std::filesystem::path& gameDirectory,
                                   const std::filesystem::path& userFolder = {});

struct PrefabEntry {
    std::filesystem::path file;
    std::string name;     // File stem.
    std::string category; // Subfolder relative to the prefab folder, '/' separated; empty at the top.
};

// Every `*.vmf` under `folder`, recursively, sorted by category then name. A missing folder
// lists nothing.
std::vector<PrefabEntry> listPrefabs(const std::filesystem::path& folder);

// Wrap a clipboard fragment as a standalone map Hammer can open, and take it apart again.
// The unwrapped form accepts Hammer's own saves too: solids and groups are read from `world`.
Result<std::string> prefabMapText(std::string_view fragment);
Result<std::string> prefabFragment(std::string_view mapText);

// Save the session's selection as `<folder>/<category>/<name>.vmf`. `name` and each part of
// `category` ('/' separated) must be plain file names. An existing file is an error unless
// `overwrite`. Returns the file written.
Result<std::filesystem::path> createPrefab(const EditorSession& session, std::string_view name,
                                           std::string_view category, const std::filesystem::path& folder,
                                           bool overwrite = false);

struct PrefabPlacement {
    // Where the prefab's bounds' bottom centre goes (risk 13), by default on a horizontal
    // plane through `at`. A surface under the cursor may supply its plane instead; the bounds
    // then rest on it, centred on `at`.
    DVec3 at{0.0};
    std::optional<SurfacePlane> plane;
    std::optional<SnapSettings> snap; // Default: the session's.
};

// One undo step; the placed objects become the selection. A failure leaves the document alone.
Result<void> placePrefabText(EditorSession& session, std::string_view fileText, const PrefabPlacement& where);
Result<void> placePrefab(EditorSession& session, const std::filesystem::path& file, const PrefabPlacement& where);

// The solids and bounds of a prefab file, for thumbnails and the placement ghost.
Result<FragmentShape> prefabShape(const std::filesystem::path& file);

// The ghost box of a prefab whose contents span `bounds`: the same size, with the bottom centre at
// the origin, which is where `placePrefab` puts it (the bounds rest on the plane through `at`).
AABB prefabLocalBounds(const AABB& bounds);

// The placement item for a prefab file: its local bounds (absent when the file cannot be read) and a
// placer that runs `placePrefab` at the surface placement's origin. Surface alignment angles are not
// applied: a prefab lands upright, as a paste does.
PrefabItem prefabItem(const std::filesystem::path& file);

} // namespace apex::editor
