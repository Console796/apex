#pragma once

#include "compile/diagnostics.hpp"
#include "document/document.hpp"

#include <optional>

namespace apex::editor {

// Looks a VMF id up at click time (0.5 D5): a compile ran on a snapshot, so the object may have been
// deleted since. Side ids resolve to their solid. Returns nullopt for "no longer in the map".
std::optional<document::NodeId> findDiagnosticObject(const document::Document& document, compile::ObjectRef object);

} // namespace apex::editor
