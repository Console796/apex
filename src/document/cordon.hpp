#pragma once

#include "core/math.hpp"
#include "document/document.hpp"

#include <optional>
#include <string>
#include <vector>

namespace apex::document {

// A VMF cordon: `cordon { "name" "active" box { "mins" "maxs" } }` under the plural `cordons` block (D7).
struct CordonBox {
    NodeId id = 0;
    std::string name;
    bool active = true;
    AABB box;
};

struct CordonSet {
    std::optional<NodeId> container;
    bool enabled = false; // The `cordons` block's own "active", as Hammer writes it.
    std::vector<CordonBox> boxes;
    // The boxes a compile is limited to: those whose own flag is on, while the block is enabled.
    std::vector<AABB> compileBoxes() const;
};

CordonSet readCordons(const Document& document);

// Each edit is one undo step. A drag passes the same coalesce key so it collapses into one.
NodeId addCordon(Document& document, std::string name, const AABB& box);
void setCordonBox(Document& document, NodeId cordon, const AABB& box, std::string coalesceKey = {});
void setCordonActive(Document& document, NodeId cordon, bool active);
void removeCordon(Document& document, NodeId cordon);

// Walls and filtering for a compile snapshot; the document is never changed (D7).
struct CordonWalls {
    std::string material = "tools/toolsskybox"; // Hammer's gameconfig CordonTexture.
    double thickness = 16.0;
};

// Keeps the world solids and entities that touch a box and appends six closing wall brushes around
// the union of the boxes. `text` is a whole VMF; the result is a whole VMF.
Result<std::string> cordonFilteredText(const std::string& text, const std::vector<AABB>& boxes,
                                       const CordonWalls& walls);

} // namespace apex::document
