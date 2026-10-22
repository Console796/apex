#pragma once

#include "editor/actions.hpp"

#include <QKeySequence>

class QKeyEvent;

namespace apex::ui {
QKeySequence keySequence(const editor::KeyChord& chord);
std::optional<editor::KeyChord> keyChord(const QKeyEvent& event);
} // namespace apex::ui
