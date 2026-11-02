#include "bake_setup.hpp"
#include "editor/bake_instances.hpp"

namespace apex::tools {
std::vector<document::InstanceSolidOrigin> expandBakeInstances(document::Document& doc, const std::filesystem::path& map,
                         const assets::GameProfile& profile) {
    return editor::expandBakeInstances(doc, map, profile.instanceSearchRoot());
}
} // namespace apex::tools
