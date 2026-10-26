#pragma once

#include "editor/tool.hpp"
#include <QWidget>
#include <map>

class QHBoxLayout;
namespace apex::ui {
class ToolParameterStrip final : public QWidget {
public:
    explicit ToolParameterStrip(QWidget* parent = nullptr);
    void update(editor::Tool& tool);
private:
    editor::Tool* tool_ = nullptr;
    QHBoxLayout* row_;
    std::vector<editor::ToolParameter> schema_;
    std::map<std::string, QWidget*> fields_;
};
} // namespace apex::ui
