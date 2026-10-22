#pragma once

#include "document/document.hpp"

#include <optional>
#include <unordered_set>

namespace apex::editor {

using document::NodeId;

struct ViewState {
    // Absent means show all. Present means show only these nodes and their descendants;
    // an empty set hides everything. Isolate never overrides file visibility.
    std::optional<std::unordered_set<NodeId>> isolate;
};

// Includes the node itself. Shared seam for a future render-mirror visibility change.
bool isHiddenByWrapper(const document::Document& document, NodeId node);
bool isVisible(const document::Document& document, NodeId node, const ViewState& view = {});

} // namespace apex::editor
