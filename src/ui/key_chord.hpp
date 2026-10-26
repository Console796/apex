#pragma once

#include "editor/actions.hpp"

#include <QKeySequence>
#include <QString>

class QKeyEvent;

namespace apex::ui {
QString shortcutText(const editor::Preset& preset, const editor::ActionId& action);
QKeySequence keySequence(const editor::KeyChord& chord);
std::optional<editor::KeyChord> keyChord(const QKeyEvent& event);
} // namespace apex::ui
