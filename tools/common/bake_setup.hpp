#pragma once

#include "assets/game_profile.hpp"
#include "assets/vfs.hpp"
#include "document/document.hpp"
#include "document/instance_contents.hpp"
#include "render/bake_setup.hpp"

#include <filesystem>

namespace apex::tools {

std::vector<document::InstanceSolidOrigin> expandBakeInstances(document::Document& doc, const std::filesystem::path& map,
                         const assets::GameProfile& profile);
using render::BakeSetup;
using render::setupBake;

} // namespace apex::tools
