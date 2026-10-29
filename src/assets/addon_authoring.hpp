#pragma once

#include "assets/addons.hpp"

namespace apex::assets {

struct AddonScaffold {
    std::string name;
    bool campaign = false;
};
// Creates a new directory only. Never replaces an existing addon or registers/mounts it implicitly.
Result<void> createAddon(const std::filesystem::path& directory, const AddonScaffold& options);
// Validates the target manifest and resolves a campaign. Empty id selects the only campaign, if any.
Result<std::string> addonCampaign(const std::filesystem::path& directory, std::string_view id = {});
// Ordered mounted directories; used to explain game/editor differences without rewriting either list.
std::vector<std::string> addonMountDifference(const AddonList& editor, const AddonList& game);

} // namespace apex::assets
