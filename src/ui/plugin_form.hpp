#pragma once
#include <apex/apex_plugin.h>
#include <QWidget>
#include <functional>
#include <string_view>

class QVBoxLayout;
namespace apex::ui {
// Plugin descriptions are borrowed; widgets copy all text and values.
class PluginForm final : public QWidget {
public:
    using Change = std::function<void(std::string_view, const apex_ui_value&)>;
    using Click = std::function<void(std::string_view)>;
    explicit PluginForm(QWidget* parent = nullptr);
    bool describe(const apex_ui_form& form);
    void setChange(Change change) { change_ = std::move(change); }
    void setClick(Click click) { click_ = std::move(click); }
private:
    void retire(QWidget* widget);
    QVBoxLayout* layout_;
    Change change_;
    Click click_;
};
}
