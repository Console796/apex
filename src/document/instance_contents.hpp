#pragma once

#include "document/document.hpp"
#include "document/instance_fixup.hpp"

#include <filesystem>

namespace apex::document {

// Hammer's viewsettings/nInstanceVisibility, also srctools.vmf.StrataInstanceVisibility.
enum class InstanceVisibility { Hidden = 0, Tinted = 1, Normal = 2 };

// Separate documents, never handles into the parent. Keys are fixed on an immutable copy;
// geometry stays template-local so placements can share the same GPU buffers.
struct InstanceContent {
    NodeId instance = 0;
    std::shared_ptr<const Document> templ;
    std::shared_ptr<const Document> keys;
    DMat4 transform{1.0};
    InstanceFixup fixup;
    std::size_t depth = 0;
    std::filesystem::path path;
};

} // namespace apex::document
