#include "ui/key_chord.hpp"

#include <QKeyEvent>

namespace apex::ui {
QKeySequence keySequence(const editor::KeyChord& chord) {
    auto key = chord.key;
    if (key == "BracketLeft") key = "[";
    if (key == "BracketRight") key = "]";
    if (key == "PageUp") key = "PgUp";      // Qt's portable names.
    if (key == "PageDown") key = "PgDown";
    if (key == "Enter") key = "Return";     // The main key; Qt's "Enter" is the keypad one.
    const auto text = std::string(chord.ctrl ? "Ctrl+" : "") + (chord.shift ? "Shift+" : "") +
                      (chord.alt ? "Alt+" : "") + key;
    return QKeySequence(QString::fromStdString(text), QKeySequence::PortableText);
}
std::optional<editor::KeyChord> keyChord(const QKeyEvent& event) {
    auto key = QKeySequence(event.key()).toString(QKeySequence::PortableText).toStdString();
    switch (event.key()) {
    case Qt::Key_Escape: key = "Escape"; break;
    case Qt::Key_Delete: key = "Delete"; break;
    case Qt::Key_Insert: key = "Insert"; break;
    case Qt::Key_PageUp: key = "PageUp"; break;
    case Qt::Key_PageDown: key = "PageDown"; break;
    case Qt::Key_Return: case Qt::Key_Enter: key = "Enter"; break;
    default: break;
    }
    auto parsed = editor::KeyChord::parse(key);
    if (!parsed) return std::nullopt;
    parsed->ctrl = event.modifiers().testFlag(Qt::ControlModifier);
    parsed->shift = event.modifiers().testFlag(Qt::ShiftModifier);
    parsed->alt = event.modifiers().testFlag(Qt::AltModifier);
    return *parsed;
}
} // namespace apex::ui
