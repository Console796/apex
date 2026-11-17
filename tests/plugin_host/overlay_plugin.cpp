// Built twice (PLUGIN_NS and OTHER_NS swapped) so the two plugins can try to use each other's overlays.
// Test plugin for apex.overlay/1, apex.helpers/1 and apex.problems/1, written against the C++ wrapper.
#include <apex/plugin.hpp>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

#ifndef PLUGIN_NS
#define PLUGIN_NS "wrap"
#define OTHER_NS "other"
#endif
namespace {
const apex_host* host = nullptr;
std::string logLine(const char* what, apex_status status) { return std::string(what) + "=" + std::to_string(status); }
void log(const std::string& text) { host->log(host->context, 1, apex::plugin::string(text)); }

apex::plugin::Overlay overlay;
struct Widget : apex::plugin::Helper {
    void draw(const apex_helper_call& call, const apex_helper_sink& sink) override {
        if (apex::plugin::view(call.arguments) == "throw") throw std::runtime_error("helper failure");
        const auto line = apex::plugin::segment(call.origin, {call.origin.x + 16, call.origin.y, call.origin.z}, 2, {1, 0, 0, 1});
        segment(sink, line);
        const auto text = apex::plugin::label(call.origin, "widget", {1, 1, 1, 1});
        label(sink, text);
    }
} widget;
struct Thrower : apex::plugin::Helper {
    void draw(const apex_helper_call&, const apex_helper_sink& sink) override {
        segment(sink, apex::plugin::segment({0, 0, 0}, {1, 1, 1}, 1, {0, 1, 0, 1})); // half-drawn output must be rolled back
        throw std::runtime_error("always");
    }
} thrower;
struct Check : apex::plugin::ProblemCheck {
    void check(const apex_problem_sink& sink) override {
        const auto* documents = static_cast<const apex_document_v1*>(host->get_interface(host->context, apex::plugin::string("apex.document/1")));
        apex_document_info info{sizeof(info)};
        documents->info(host->context, &info);
        report(sink, 2, "roots=" + std::to_string(info.root_count));
        apex_node_id first = 0;
        if (info.root_count && documents->root_at(host->context, 0, &first) == APEX_OK)
            report(sink, 1, "first root", {first});
        logBad(sink);
    }
    static void logBad(const apex_problem_sink& sink) {
        apex::plugin::ProblemCheck::report(sink, 9, "bad severity"); // rejected, not fatal
        log(logLine("bad_severity", apex::plugin::ProblemCheck::report(sink, 9, "x")));
        log(logLine("unknown_object", apex::plugin::ProblemCheck::report(sink, 0, "x", {987654})));
    }
} check;
struct Broken : apex::plugin::ProblemCheck {
    void check(const apex_problem_sink& sink) override {
        report(sink, 0, "half a result");
        throw std::runtime_error("check failure");
    }
} broken;

void setMain() {
    overlay.clearLists();
    overlay.segments.push_back(apex::plugin::segment({0, 0, 0}, {64, 0, 0}, 3, {0, 1, 0, 1}));
    overlay.points.push_back(apex::plugin::point({0, 0, 0}, 6, {1, 1, 0, 1}));
    overlay.labels.push_back(apex::plugin::label({0, 0, 8}, "main", {1, 1, 1, 1}));
    overlay.tints.push_back(apex::plugin::tint(5, {1, 0, 0, 0.5f}));
    static const apex_plane cube[6] = {{{1, 0, 0}, 8}, {{-1, 0, 0}, 8}, {{0, 1, 0}, 8}, {{0, -1, 0}, 8}, {{0, 0, 1}, 8}, {{0, 0, -1}, 8}};
    overlay.ghosts.push_back(apex::plugin::overlayGhost(cube, 6, {0, 0.5f, 1, 0.3f}));
}
void act(void*) {
    // Callback into the plugin: set/clear work, ownership and validation are enforced.
    setMain();
    log(logLine("set", apex::plugin::setOverlay(*host, "plugin." PLUGIN_NS ".main", overlay)));
    apex::plugin::Overlay bad;
    bad.points.push_back(apex::plugin::point({std::nan(""), 0, 0}, 4, {1, 1, 1, 1}));
    log(logLine("set_nan", apex::plugin::setOverlay(*host, "plugin." PLUGIN_NS ".main", bad)));
    log(logLine("set_unknown", apex::plugin::setOverlay(*host, "plugin." PLUGIN_NS ".nothing", overlay)));
    log(logLine("steal", apex::plugin::setOverlay(*host, "plugin." OTHER_NS ".main", overlay)));
    log(logLine("declare_late", apex::plugin::declareOverlay(*host, "plugin." PLUGIN_NS ".late", "Late")));
    log(logLine("helper_late", apex::plugin::registerHelper(*host, "latewidget", widget)));
}
void clearAct(void*) { log(logLine("clear", apex::plugin::clearOverlay(*host, "plugin." PLUGIN_NS ".main"))); }
void throwing(void*) { throw std::runtime_error("callback failure"); }

apex_status activate(void*) {
    using namespace apex::plugin;
    auto status = declareOverlay(*host, "plugin." PLUGIN_NS ".main", "Wrap overlay");
    if (status != APEX_OK) return status;
    status = declareOverlay(*host, "plugin." PLUGIN_NS ".other", "Wrap second overlay");
    if (status != APEX_OK) return status;
    log(logLine("declare_dup", declareOverlay(*host, "plugin." PLUGIN_NS ".main", "Again")));
    log(logLine("declare_outside", declareOverlay(*host, "plugin.elsewhere.main", "Nope")));
    setMain();
    if ((status = setOverlay(*host, "plugin." PLUGIN_NS ".main", overlay)) != APEX_OK) return status;
    if ((status = registerHelper(*host, PLUGIN_NS "widget", widget)) != APEX_OK) return status;
    if ((status = registerHelper(*host, PLUGIN_NS "thrower", thrower)) != APEX_OK) return status;
    log(logLine("helper_builtin", registerHelper(*host, "light", widget)));
    log(logLine("helper_dup", registerHelper(*host, PLUGIN_NS "widget", widget)));
    if ((status = registerProblemCheck(*host, "plugin." PLUGIN_NS ".check", "Wrap check", check)) != APEX_OK) return status;
    if ((status = registerProblemCheck(*host, "plugin." PLUGIN_NS ".broken", "Wrap broken check", broken)) != APEX_OK) return status;
    log(logLine("problem_outside", registerProblemCheck(*host, "plugin.elsewhere.check", "Nope", check)));
    const auto* registry = actions(*host);
    if (!registry) return APEX_ERR_UNSUPPORTED;
    registry->register_action(host->context, string("plugin." PLUGIN_NS ".act"), string("Wrap act"), string("Sets overlays"), nullptr, act, nullptr, 0);
    registry->register_action(host->context, string("plugin." PLUGIN_NS ".clear"), string("Wrap clear"), string("Clears overlays"), nullptr, clearAct, nullptr, 0);
    registry->register_action(host->context, string("plugin." PLUGIN_NS ".throw"), string("Wrap throw"), string("Throws"), nullptr, throwing, nullptr, 0);
    return APEX_OK;
}
const apex_plugin plugin{sizeof(apex_plugin), APEX_PLUGIN_ABI_MAJOR, APEX_PLUGIN_ABI_MINOR, 0, nullptr, activate, nullptr, nullptr, nullptr, nullptr};
}
extern "C" APEX_PLUGIN_EXPORT const apex_plugin* apex_plugin_entry(const apex_host* h) { host = h; return &plugin; }
