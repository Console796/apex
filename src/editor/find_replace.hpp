#pragma once

#include "editor/asset_report.hpp"
#include "editor/editor_session.hpp"

#include <set>

namespace apex::editor {

struct TextReplaceOptions {
    std::string find, replacement;
    bool wholeWord = false, caseSensitive = false, wildcard = false;
};
struct KeyvalueReplaceOptions : TextReplaceOptions {
    bool keys = true, values = true, connections = false;
};
struct KeyvalueChange {
    NodeId entity;
    std::string key, newKey, oldValue, newValue;
    std::size_t index = 0; // scalar index in entity or connections block
    std::optional<NodeId> connectionBlock;
    bool operator==(const KeyvalueChange&) const = default;
};
// Empty entity set means no entities. '*' greedily matches zero or more bytes;
// replacement is literal. Whole-word uses ASCII letters/digits/underscore boundaries.
// Identity keys are excluded. Malformed connections are left untouched. Connection
// replacements containing their separator are refused. Plain text edits of targetname
// do not propagate references; use planRename for semantic renames (0.4 D14/D6).
Result<std::vector<KeyvalueChange>> previewKeyvalueReplace(const document::Document& doc,
    const std::set<NodeId>& entities, const KeyvalueReplaceOptions& options);
// Applies exactly the reviewed snapshot; stale previews fail atomically. One undo step.
Result<void> applyKeyvalueReplace(EditorSession& session, std::span<const KeyvalueChange> changes);

enum class MaterialReplaceScope { Selection, WholeMap };
struct MaterialReplaceOptions {
    std::string find, replacement;
    bool wholeName = true, entityKeys = false;
    MaterialReplaceScope scope = MaterialReplaceScope::Selection;
};
struct MaterialChange {
    NodeId object;
    std::optional<SideId> side;
    std::string key, oldValue, newValue;
    std::size_t index = 0; // entity scalar index, unused for faces
    bool operator==(const MaterialChange&) const = default;
};
// Material names compare case-insensitively with either slash. Selection expands
// groups/brushes using expandSelection; face mode touches only its selected faces.
// Entity keys in selection scope require the entity itself to be selected/expanded.
Result<std::vector<MaterialChange>> previewMaterialReplace(const EditorSession& session,
    const MaterialReplaceOptions& options);
Result<void> applyMaterialReplace(EditorSession& session, std::span<const MaterialChange> changes);

} // namespace apex::editor
