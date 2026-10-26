#pragma once

#include "editor/editor_session.hpp"

#include <QDialog>
#include <QWidget>

#include <functional>

namespace apex::ui {

using RenameDialogPresenter = std::function<bool(QDialog*)>;
bool renameWithConfirmation(editor::EditorSession& session, document::NodeId entity,
                            std::string name, QWidget* parent,
                            const RenameDialogPresenter& presenter = {});

} // namespace apex::ui
