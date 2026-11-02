#include "bake_setup.hpp"
#include "editor/bake_instances.hpp"

namespace apex::tools {
void expandBakeInstances(document::Document& doc, const std::filesystem::path& map,
                         const assets::GameProfile& profile) {
    editor::expandBakeInstances(doc, map, profile.instanceSearchRoot());
}
} // namespace apex::tools
