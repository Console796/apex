#pragma once

#include "core/kv1.hpp"
#include "document/document.hpp"
#include "geometry/displacement.hpp"

#include <optional>

namespace apex::document {

// D1: a side's dispinfo block stays the truth; these convert it on demand. Missing keys and
// rows read as zero (power must be present). Unknown keys and rows are ignored here and kept
// by writeDisplacement.
Result<geometry::Displacement> parseDisplacement(const kv1::Node& dispinfo);

// Rewrites only the scalars and rows whose parsed values differ from value, so writing back an
// unmodified parse leaves the block byte-identical and a one-row edit changes one row. Rows a
// smaller power no longer uses are removed. Rejects an invalid value without writing.
Result<void> writeDisplacement(kv1::Node& dispinfo, const geometry::Displacement& value);

// The displacement of a side, or nullopt for a flat side or unknown ID. A dispinfo that does not
// parse is an error. Parsed on each call (no cache yet).
Result<std::optional<geometry::Displacement>> displacement(const Document& doc, SideId side);

} // namespace apex::document
