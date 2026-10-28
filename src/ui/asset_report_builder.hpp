#pragma once

#include "editor/editor_session.hpp"
#include "editor/map_problems.hpp"

#include <memory>

namespace apex::ui {
class GameContent;

// Where an asset kind lives in the game's file system, when it is a plain file (0.4 D11).
std::optional<std::string> assetVfsPath(editor::AssetKind kind, std::string_view name);

// The 0.4 asset report resolved against the game's content. Names are as written in the map; each
// kind resolves differently. Sounds and particles come from the browsers' indexes, and an index still
// building counts as present rather than flooding the report with false misses. Without content,
// everything counts as present.
std::unique_ptr<editor::AssetReport> buildAssetReport(const document::Document& document,
                                                      GameContent* content, const editor::EditorSession* session);

// Runs the built-in checks over the document (0.5 D8). Asset, connection and class checks need the game's
// content; they are skipped without it.
std::vector<editor::Problem> collectMapProblems(const document::Document& document, GameContent* content,
                                                const editor::EditorSession* session);

} // namespace apex::ui
