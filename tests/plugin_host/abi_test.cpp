// I-7 findings 5-11: struct_size arrays, namespace boundaries, id validation, per-pane ghosts, key names and
// gesture ownership. The compile-time layout audit is abi_layout_c.c and abi_layout_cpp.cpp.
#include "plugin_host/abi_array.hpp"
#include "plugin_host/plugin_host.hpp"
#include "plugin_host/plugin_tool.hpp"
#include "editor/editor_session.hpp"
#include <doctest/doctest.h>
#include <cstring>
#include <map>
#include <vector>

extern "C" int apex_abi_layout_count_c();
extern "C" int apex_abi_layout_count_cpp();

using namespace apex;
using namespace apex::plugin_host;

TEST_CASE("ABI layout lists agree between C and C++") {
    CHECK(apex_abi_layout_count_c() > 40);
    CHECK(apex_abi_layout_count_c() == apex_abi_layout_count_cpp());
}

namespace {
// What a plugin built against a header with one extra trailing field in apex_overlay_tint would pass.
struct NewerTint { uint32_t struct_size; int32_t side; float rgba[4]; uint64_t extra; };
}

TEST_CASE("arrays of structs are read at the element's own struct_size") {
    SUBCASE("current layout") {
        apex_overlay_tint tints[3]{};
        for (int i = 0; i < 3; ++i) { tints[i].struct_size = sizeof(apex_overlay_tint); tints[i].side = i + 1; tints[i].rgba[3] = 0.5f; }
        const AbiArray<apex_overlay_tint> array(tints, 3);
        REQUIRE(array.valid());
        REQUIRE(array.size() == 3);
        CHECK(array[2].side == 3);
        CHECK(array[1].rgba[3] == 0.5f);
    }
    SUBCASE("a newer, larger element stride is walked and the unknown tail ignored") {
        static_assert(sizeof(NewerTint) > sizeof(apex_overlay_tint));
        NewerTint tints[3]{};
        for (int i = 0; i < 3; ++i) { tints[i] = {sizeof(NewerTint), 10 + i, {1, 0, 0, 1}, 99}; }
        const AbiArray<apex_overlay_tint> array(reinterpret_cast<const apex_overlay_tint*>(tints), 3);
        REQUIRE(array.valid());
        CHECK(array[0].side == 10);
        CHECK(array[2].side == 12);  // read with the plugin's stride, not the host's
        CHECK(array[2].rgba[0] == 1.0f);
    }
    SUBCASE("a smaller element: fields it does not cover read as zero") {
        // A struct older than the current one: the host pinned the 1.0 size at the offset of rgba; a plugin that stops there has no colour.
        const std::size_t base = offsetof(apex_overlay_tint, rgba);
        static_assert(sizeof(apex_overlay_tint) % alignof(apex_overlay_tint) == 0);
        std::vector<unsigned char> bytes(base * 2, 0);
        for (int i = 0; i < 2; ++i) {
            const uint32_t size = static_cast<uint32_t>(base); const int32_t side = 20 + i;
            std::memcpy(bytes.data() + i * base, &size, 4); std::memcpy(bytes.data() + i * base + 4, &side, 4);
        }
        const AbiArray<apex_overlay_tint> array(reinterpret_cast<const apex_overlay_tint*>(bytes.data()), 2, base);
        REQUIRE(array.valid());
        CHECK(array[1].side == 21);
        CHECK(array[1].rgba[0] == 0.0f);
        CHECK(array[1].struct_size == base);
        // Below the base size the array is refused.
        const AbiArray<apex_overlay_tint> refused(reinterpret_cast<const apex_overlay_tint*>(bytes.data()), 2);
        CHECK_FALSE(refused.valid());
    }
    SUBCASE("zero, mismatched, unaligned or oversized struct_size is refused") {
        apex_overlay_tint tints[2]{};
        tints[0].struct_size = sizeof(apex_overlay_tint); tints[1].struct_size = sizeof(apex_overlay_tint);
        CHECK(AbiArray<apex_overlay_tint>(tints, 2).valid());
        tints[1].struct_size = 0;  // the "array of structs without struct_size" of the old ABI
        CHECK_FALSE(AbiArray<apex_overlay_tint>(tints, 2).valid());
        tints[1].struct_size = sizeof(apex_overlay_tint) + 8;  // differs from the first element's
        CHECK_FALSE(AbiArray<apex_overlay_tint>(tints, 2).valid());
        tints[0].struct_size = tints[1].struct_size = sizeof(apex_overlay_tint) + 1;  // not a multiple of the alignment
        CHECK_FALSE(AbiArray<apex_overlay_tint>(tints, 2).valid());
        tints[0].struct_size = tints[1].struct_size = 1u << 20;
        CHECK_FALSE(AbiArray<apex_overlay_tint>(tints, 2).valid());
        CHECK_FALSE(AbiArray<apex_overlay_tint>(nullptr, 1).valid());
        CHECK(AbiArray<apex_overlay_tint>(nullptr, 0).valid());
    }
    SUBCASE("out structs honour the caller's struct_size") {
        apex_document_info big[2]{};
        apex_document_info value{sizeof(apex_document_info), 1, 42, 3};
        big[0].struct_size = sizeof(apex_document_info);
        big[1].revision = 0xdeadbeef;  // after the struct: must stay untouched
        CHECK(writeOut(&big[0], value));
        CHECK(big[0].revision == 42);
        CHECK(big[1].revision == 0xdeadbeef);
        apex_document_info small{};
        small.struct_size = offsetof(apex_document_info, revision);  // base size is larger: refused
        CHECK_FALSE(writeOut(&small, value));
        apex_document_info zero{};
        CHECK_FALSE(writeOut(&zero, value));
    }
}

TEST_CASE("registration ids match on the plugin boundary") {
    CHECK(inNamespace("com.example", "plugin.com.example.tool"));
    CHECK(inNamespace("com.example.paint", "plugin.com.example.paint.brush"));
    // com.example must not claim com.example.paint's ids, and the shorter id owns nothing below the longer one.
    CHECK_FALSE(inNamespace("com.example", "plugin.com.example.paint.brush"));
    CHECK_FALSE(inNamespace("com.example.paint", "plugin.com.example.brush"));
    CHECK_FALSE(inNamespace("com.example", "plugin.com.examples.tool"));
    CHECK_FALSE(inNamespace("com.example", "plugin.com.example"));
    CHECK_FALSE(inNamespace("com", "plugin.com.example.tool"));
}

TEST_CASE("malformed ids are rejected with APEX_ERR_INVALID before registration") {
    CHECK_FALSE(inNamespace("example", "plugin.example."));        // trailing dot
    CHECK_FALSE(inNamespace("example", "plugin.example..tool"));
    CHECK_FALSE(inNamespace("example", "plugin.example.a.b"));      // the name is one segment
    CHECK_FALSE(inNamespace("example", "plugin.example.tool "));
    CHECK_FALSE(inNamespace("example", "plugin.example.to/ol"));
    CHECK_FALSE(inNamespace("example", ""));
    CHECK_FALSE(inNamespace("example.", "plugin.example..tool"));
    CHECK_FALSE(inNamespace("", "plugin..tool"));
    CHECK(inNamespace("example", "plugin.example.tool_1-b"));
    CHECK(validPluginId("com.example.paint"));
    CHECK_FALSE(validPluginId("com..example"));
    CHECK_FALSE(validPluginId(".example"));
    CHECK_FALSE(validPluginId("example."));
    CHECK_FALSE(validPluginId("exa mple"));
    CHECK_FALSE(validPluginId(""));
    CHECK_FALSE(validItemName("a.b"));
    CHECK_FALSE(validItemName(""));
}

namespace {
struct Probe {
    std::vector<std::string> keys;
    std::vector<uint32_t> flags;
    std::vector<uint64_t> sources;
    std::map<uint64_t, int> ghostsFor;  // source -> ghosts to return
    bool handle = true;
};
const apex_plane kCube[6] = {{{1, 0, 0}, 8}, {{-1, 0, 0}, 8}, {{0, 1, 0}, 8}, {{0, -1, 0}, 8}, {{0, 0, 1}, 8}, {{0, 0, -1}, 8}};
apex_tool_ghost gGhost{sizeof(apex_tool_ghost), 0.5f, kCube, 6, {}};
apex_tool_callbacks probeCallbacks() {
    apex_tool_callbacks c{};
    c.struct_size = sizeof(c);
    c.input = [](void* user, const apex_tool_event* e, const apex_tool_view*) -> int {
        auto& p = *static_cast<Probe*>(user);
        p.keys.emplace_back(e->key.data ? std::string(e->key.data, e->key.size) : std::string{});
        p.flags.push_back(e->flags); p.sources.push_back(e->source);
        return p.handle ? 1 : 0;
    };
    c.visual = [](void* user, uint64_t source, apex_tool_visual* out) -> apex_status {
        auto& p = *static_cast<Probe*>(user);
        const auto n = p.ghostsFor[source];
        out->ghosts = n ? &gGhost : nullptr; out->ghost_count = static_cast<size_t>(n);
        return APEX_OK;
    };
    return c;
}
editor::InputEvent pointer(editor::InputKind kind, uint64_t source, bool left) {
    editor::InputEvent e; e.kind = kind; e.source = source; e.left = left; e.button = editor::PointerButton::Left;
    return e;
}
const auto kGuard = [](const char*, const std::function<void()>& fn) { fn(); };
}

TEST_CASE("one pane's input does not overwrite another pane's ghost preview") {
    editor::EditorSession session;
    Probe probe;
    PluginTool tool(session, "plugin.test.tool", &probe, probeCallbacks(), kGuard);
    editor::ToolView view;
    probe.ghostsFor[1] = 1;
    tool.input(pointer(editor::InputKind::Move, 1, false), view);
    REQUIRE(tool.preview().ghosts().size() == 1);
    // Pane 2 has nothing to draw: pane 1's ghost stays.
    probe.ghostsFor[2] = 0;
    tool.input(pointer(editor::InputKind::Move, 2, false), view);
    CHECK(tool.preview().ghosts().size() == 1);
    // Pane 2 draws its own: both show.
    probe.ghostsFor[2] = 1;
    tool.input(pointer(editor::InputKind::Move, 2, false), view);
    CHECK(tool.preview().ghosts().size() == 2);
    // Closing pane 1 removes only its ghost.
    tool.forget(1);
    CHECK(tool.preview().ghosts().size() == 1);
    tool.cancel();
    CHECK(tool.preview().ghosts().empty());
}

TEST_CASE("plugin tools receive the canonical key name") {
    editor::EditorSession session;
    Probe probe;
    PluginTool tool(session, "plugin.test.tool", &probe, probeCallbacks(), kGuard);
    editor::ToolView view;
    editor::InputEvent e;
    e.kind = editor::InputKind::KeyDown; e.source = 1;
    e.key = "";  // the modal-transform name is empty for keys it does not know
    e.chord = editor::KeyChord{"Enter", false, true, false};
    e.shift = true;
    tool.input(e, view);
    editor::InputEvent bare;
    bare.kind = editor::InputKind::KeyDown; bare.source = 1; bare.modifierKey = true; bare.chord = editor::KeyChord{"Shift", false, true, false};
    tool.input(bare, view);
    editor::InputEvent fallback;
    fallback.kind = editor::InputKind::KeyDown; fallback.source = 1; fallback.key = "Escape";
    tool.input(fallback, view);
    REQUIRE(probe.keys.size() == 3);
    CHECK(probe.keys[0] == "Enter");
    CHECK(probe.keys[1].empty());  // a bare modifier has no name, only the flag
    CHECK((probe.flags[1] & APEX_EVENT_MODIFIER_KEY) != 0);
    CHECK(probe.keys[2] == "Escape");
}

TEST_CASE("a handled press owns its pane until the buttons are up") {
    editor::EditorSession session;
    Probe probe;
    PluginTool tool(session, "plugin.test.tool", &probe, probeCallbacks(), kGuard);
    editor::ToolView view;
    CHECK_FALSE(tool.owns(1));
    probe.handle = false;
    tool.input(pointer(editor::InputKind::Press, 1, true), view);
    CHECK_FALSE(tool.owns(1));  // a press the plugin declined is not owned
    probe.handle = true;
    tool.input(pointer(editor::InputKind::Press, 1, true), view);
    CHECK(tool.owns(1));
    CHECK_FALSE(tool.owns(2));
    tool.input(pointer(editor::InputKind::Move, 1, true), view);
    CHECK(tool.owns(1));  // still dragging
    tool.input(pointer(editor::InputKind::Release, 1, false), view);
    CHECK_FALSE(tool.owns(1));
    tool.input(pointer(editor::InputKind::Press, 2, true), view);
    REQUIRE(tool.owns(2));
    tool.input(pointer(editor::InputKind::Cancel, 2, false), view);
    CHECK_FALSE(tool.owns(2));
    tool.input(pointer(editor::InputKind::Press, 3, true), view);
    REQUIRE(tool.owns(3));
    tool.forget(3);
    CHECK_FALSE(tool.owns(3));
}
