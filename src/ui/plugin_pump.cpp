#include "ui/plugin_pump.hpp"
#include "plugin_host/plugin_host.hpp"

namespace apex::ui {
PluginPump::PluginPump(plugin_host::Host& host, QObject* parent, int intervalMs) : QObject(parent), host_(host), timer_(this) {
    timer_.setInterval(intervalMs);
    connect(&timer_, &QTimer::timeout, this, &PluginPump::pump);
    timer_.start();
}
void PluginPump::pump() {
    host_.drainJobs();
    if (host_.hasWatches()) host_.pollWatches();
}
}
