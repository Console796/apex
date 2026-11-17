// Test plugin for apex.jobs/1 and apex.watch/1, written against the C++ wrapper.
// Actions start work; results are logged from the editor thread so the test can inspect them.
#include <apex/plugin.hpp>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
const apex_host* host = nullptr;
void log(const std::string& text) { host->log(host->context, 1, apex::plugin::string(text)); }
std::string threadId() { return std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())); }

struct TestJob : apex::plugin::Job {
    std::string name;
    std::string workThread;
    bool fail = false;
    bool slow = false;
    apex_status run(const apex_job_context& context) override {
        workThread = threadId();
        if (fail) throw std::runtime_error("work failure");
        for (int i = 0; slow && i < 20 && !cancelled(context); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return APEX_OK;
    }
    void done(apex_status status) override {
        log("done:" + name + ":status=" + std::to_string(status) + ":work=" + workThread + ":main=" + threadId());
        if (name == "throwing") throw std::runtime_error("completion failure");
    }
};
TestJob plain, failing, throwing, cancelled;

void startJob(void*) {
    plain.name = "plain";
    log("submit=" + std::to_string(apex::plugin::submitJob(*host, plain)) + ":main=" + threadId());
}
void startFailing(void*) { failing.name = "failing"; failing.fail = true; apex::plugin::submitJob(*host, failing); }
void startThrowing(void*) { throwing.name = "throwing"; apex::plugin::submitJob(*host, throwing); }
void startCancelled(void*) {
    cancelled.name = "cancelled"; cancelled.slow = true;
    uint64_t id = 0;
    log("submit=" + std::to_string(apex::plugin::submitJob(*host, cancelled, &id)));
    log("cancel=" + std::to_string(apex::plugin::cancelJob(*host, id)));
    log("cancel_again=" + std::to_string(apex::plugin::cancelJob(*host, id)));
}

struct TestWatcher : apex::plugin::Watcher {
    void changed(std::string_view path) override {
        log("changed:" + std::string(path) + ":main=" + threadId());
        if (path.ends_with("boom.txt")) throw std::runtime_error("watch failure");
    }
} watcher;
uint64_t watchId = 0;
void startWatch(void*) {
    const char* path = std::getenv("APEX_TEST_WATCH");
    log("watch=" + std::to_string(apex::plugin::watchPath(*host, path ? path : "", watcher, &watchId)));
}
void startWatchRelative(void*) {
    uint64_t id = 0;
    log("watch_relative=" + std::to_string(apex::plugin::watchPath(*host, "relative/file.txt", watcher, &id)));
    log("watch_outside=" + std::to_string(apex::plugin::watchPath(*host, "/etc/hostname", watcher, &id)));
}
void stopWatch(void*) { log("unwatch=" + std::to_string(apex::plugin::unwatch(*host, watchId))); }

apex_status activate(void*) {
    using namespace apex::plugin;
    const auto* registry = actions(*host);
    if (!registry) return APEX_ERR_UNSUPPORTED;
    const auto add = [&](const char* id, void (*fn)(void*)) {
        registry->register_action(host->context, string(id), string(id), string("test"), nullptr, fn, nullptr, 0);
    };
    add("plugin.async.job", startJob);
    add("plugin.async.failing", startFailing);
    add("plugin.async.throwing", startThrowing);
    add("plugin.async.cancel", startCancelled);
    add("plugin.async.watch", startWatch);
    add("plugin.async.watch_bad", startWatchRelative);
    add("plugin.async.unwatch", stopWatch);
    // Registering at activate is allowed for watch too: the path is the plugin's own folder.
    return APEX_OK;
}
const apex_plugin plugin{sizeof(apex_plugin), APEX_PLUGIN_ABI_MAJOR, APEX_PLUGIN_ABI_MINOR, nullptr, activate, nullptr, nullptr, nullptr, nullptr};
}
extern "C" APEX_PLUGIN_EXPORT const apex_plugin* apex_plugin_entry(const apex_host* h) { host = h; return &plugin; }
