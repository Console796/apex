#include "corpus.hpp"
#include "editor/editor_session.hpp"

#include <set>

using namespace editor_test;

namespace {
using Identity = std::pair<std::string, int>;
std::set<Identity> identities(const Document& doc) {
    std::set<Identity> result;
    const auto file = doc.toVmf();
    REQUIRE(file);
    const auto walk = [&](const auto& self, const kv1::Node& node) -> void {
        if (node.kind() == kv1::NodeKind::Block) {
            if (const auto* value = node.find("id")) {
                REQUIRE(value->asInt());
                CHECK(result.emplace(std::string(node.key()), *value->asInt()).second);
            }
        }
        for (const auto& child : node.children()) self(self, child);
    };
    walk(walk, file->kvDocument().root());
    return result;
}
std::vector<std::string> fields(const kv1::Node& node) {
    if (node.key() == "id" || node.key() == "groupid") return {};
    std::vector<std::string> result{std::string(node.rawKey()), std::string(node.rawValue().value_or("")), std::string(node.condition())};
    for (const auto& child : node.children()) {
        const auto values = fields(child);
        result.insert(result.end(), values.begin(), values.end());
    }
    result.emplace_back("end");
    return result;
}
}

TEST_CASE("[corpus] Clipboard repeated in-place paste remaps all identities and undoes cut exactly") {
    const auto paths = corpusPaths();
    if (paths.empty()) { MESSAGE("APEX_P2CE_DIR is unset; skipping clipboard corpus"); return; }
    std::size_t tested = 0;
    for (const auto& path : paths) {
        CAPTURE(path.string());
        Document doc(readCorpus(path));
        NodeId chosen = 0;
        for (const auto h : nodes(doc)) {
            if (std::holds_alternative<Handle<Solid>>(h) && !brushEntityOf(doc, id(doc, h)) && isVisible(doc, id(doc, h))) {
                chosen = id(doc, h);
                break;
            }
        }
        if (!chosen) continue;
        EditorSession session;
        session.attach(doc);
        session.selection().setMode(SelectionMode::Solids);
        session.selection().setIgnoreGroups(true);
        session.selection().select(chosen);
        const auto before = doc.write().value();
        const auto original = doc.exportSubtree(chosen);
        const auto text = session.copyText();
        REQUIRE(text);
        auto previous = identities(doc);
        const auto count = original.findAll("side").size() + 1;
        for (int copy = 0; copy < 3; ++copy) {
            REQUIRE(session.pasteText(*text, {.inPlace = true}));
            REQUIRE(session.selection().ordered().size() == 1);
            const auto clone = std::get<NodeId>(session.selection().ordered().front());
            CHECK(clone != chosen);
            CHECK(fields(doc.exportSubtree(clone)) == fields(original));
            const auto now = identities(doc);
            CHECK(now.size() == previous.size() + count);
            CHECK(std::ranges::includes(now, previous));
            previous = now;
        }
        const auto pasted = doc.write().value();
        const auto cut = session.cutText();
        REQUIRE(cut);
        CHECK(identities(doc).size() == previous.size() - count);
        REQUIRE(session.perform("edit.undo"));
        CHECK(doc.write().value() == pasted);
        REQUIRE(session.jumpHistory(0));
        CHECK(doc.write().value() == before);
        REQUIRE(session.jumpHistory(3));
        CHECK(doc.write().value() == pasted);
        if (++tested == 3) break;
    }
    REQUIRE(tested == 3);
}
