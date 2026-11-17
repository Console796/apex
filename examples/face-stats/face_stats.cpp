// Face stats: an apex.ui/1 section under Face Properties that summarises the selected faces.
// It shows how a plugin
//   - registers a section for a host panel (registerSection with the "face_properties" target),
//   - reads the context the host passes to describe() (the selected side IDs),
//   - reads the open map through apex.document/1, and
//   - asks for a re-describe when the map changes (refresh from on_changed).
// Nothing here throws across the ABI: Form::callbacks() already catches, and on_changed catches itself.
#include <apex/plugin.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace {
const apex_host* host = nullptr;
constexpr const char* sectionId = "plugin.facestats.stats";

// Counts the nodes of one kind under every root of the open map; false when no map is open.
bool countNodes(const apex_host& h, std::string_view kind, size_t& count) {
    const auto* document = static_cast<const apex_document_v1*>(h.get_interface(h.context, apex::plugin::string("apex.document/1")));
    apex_document_info info{}; info.struct_size = sizeof(info);
    if (!document || document->info(h.context, &info) != APEX_OK || !info.is_open) return false;
    std::vector<apex_node_id> pending;
    for (size_t i = 0; i < info.root_count; ++i) {
        apex_node_id root = 0;
        if (document->root_at(h.context, i, &root) == APEX_OK) pending.push_back(root);
    }
    count = 0;
    while (!pending.empty()) {
        const auto id = pending.back(); pending.pop_back();
        apex_node_info node{}; node.struct_size = sizeof(node);
        if (document->node(h.context, id, &node) != APEX_OK) continue;
        if (apex::plugin::view(node.kind) == kind) ++count;
        for (size_t i = 0; i < node.child_count; ++i) {
            apex_node_id child = 0;
            if (document->child_at(h.context, id, i, &child) == APEX_OK) pending.push_back(child);
        }
    }
    return true;
}

class Stats final : public apex::plugin::Form {
public:
    void describe(const apex_ui_context* context, std::vector<apex_ui_control>& controls) override {
        const bool sides = context && apex::plugin::view(context->kind) == "sides";
        const size_t faces = sides ? context->id_count : 0;
        rows_[0] = faces ? "Faces selected: " + std::to_string(faces) : "Select faces to see their stats";
        rows_[1].clear(); rows_[2].clear();
        if (faces) {
            const auto [low, high] = std::minmax_element(context->ids, context->ids + faces);
            rows_[1] = "Side IDs: " + std::to_string(*low) + (*low == *high ? "" : " to " + std::to_string(*high));
            size_t solids = 0;
            rows_[2] = countNodes(*host, "solid", solids) ? "Solids in the map: " + std::to_string(solids) : "No map is open";
        }
        for (size_t i = 0; i < rows_.size(); ++i) {
            if (rows_[i].empty()) continue;
            auto label = Form::control(APEX_UI_LABEL, keys[i], "");
            label.value.text = apex::plugin::string(rows_[i]);
            controls.push_back(label);
        }
    }
private:
    // The host borrows the text until the next describe(), so it lives here.
    std::array<std::string, 3> rows_;
    static constexpr std::array<const char*, 3> keys{"row0", "row1", "row2"};
};
Stats stats;

apex_status activate(void*) {
    return apex::plugin::registerSection(*host, sectionId, "Face stats", "face_properties", stats);
}
void changed(void*, const apex_change_notice*) {
    try { apex::plugin::refresh(*host, sectionId); } catch (...) {}
}
const apex_plugin plugin{sizeof(apex_plugin), APEX_PLUGIN_ABI_MAJOR, APEX_PLUGIN_ABI_MINOR, 0, nullptr, activate, nullptr, nullptr, nullptr, changed};
}
extern "C" APEX_PLUGIN_EXPORT const apex_plugin* apex_plugin_entry(const apex_host* h) { host = h; return &plugin; }
