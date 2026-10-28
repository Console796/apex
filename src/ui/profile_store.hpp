#pragma once

#include "compile/sequence.hpp"

#include <QByteArray>
#include <vector>

namespace apex::ui {

// User compile profiles live in the preferences as JSON, never in the VMF (0.5 D2).
QByteArray profilesToJson(const std::vector<compile::CompileProfile>& profiles);
// Tolerant: entries that are not objects or lack a name are dropped; unknown special commands are ignored.
std::vector<compile::CompileProfile> profilesFromJson(const QByteArray& json);
std::vector<compile::CompileProfile> loadUserProfiles();
void saveUserProfiles(const std::vector<compile::CompileProfile>& profiles);

} // namespace apex::ui
