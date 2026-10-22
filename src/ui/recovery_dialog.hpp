#pragma once

#include "editor/autosave.hpp"

#include <QDialog>
#include <functional>

class QListWidget;

namespace apex::ui {

class RecoveryDialog : public QDialog {
public:
    enum class Choice { Open, Discard, Later };
    using Open = std::function<bool(const editor::Recovery&)>;
    RecoveryDialog(std::vector<editor::Recovery> recoveries, Open open, QWidget* parent = nullptr);
    // The same controller path is callable without running a modal event loop.
    bool choose(std::size_t index, Choice choice);
    std::size_t count() const { return recoveries_.size(); }
private:
    std::vector<editor::Recovery> recoveries_;
    Open open_;
    QListWidget* list_;
};

} // namespace apex::ui
