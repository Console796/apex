#pragma once
#include <QDialog>
namespace apex::plugin_host { class Host; }
namespace apex::ui {
class PluginManager final : public QDialog {
public:
    explicit PluginManager(plugin_host::Host& host, QWidget* parent = nullptr);
};
}
