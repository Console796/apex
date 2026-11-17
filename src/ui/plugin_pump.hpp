#pragma once
// The editor-thread dispatch for plugin background work: a timer that delivers finished apex.jobs/1
// completions and polls apex.watch/1 watches. The plugin host itself is Qt-free; this is its only clock.
#include <QObject>
#include <QTimer>

namespace apex::plugin_host { class Host; }

namespace apex::ui {
class PluginPump : public QObject {
    Q_OBJECT
public:
    explicit PluginPump(plugin_host::Host& host, QObject* parent = nullptr, int intervalMs = 50);
    // One delivery pass; the timer calls this, tests may too.
    void pump();
private:
    plugin_host::Host& host_;
    QTimer timer_;
};
}
