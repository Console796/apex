#include "fixtures.hpp"
#include "core/strings.hpp"
#include "editor/connection_model.hpp"
#include "editor/editor_session.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace editor_test;

namespace {
std::string text(const Document& doc) {
    auto result = doc.write();
    REQUIRE(result);
    return *result;
}
Selection selected(const Document& doc) {
    Selection selection(SelectionMode::Objects);
    for (const auto root : doc.roots()) selection.add(Target{root});
    return selection;
}
const kv1::Node* connections(const Document& doc, NodeId entity) {
    for (const auto id : doc.get(*doc.handle(entity))->children) {
        const auto* block = doc.get(*doc.handle(id))->block.get();
        if (ciEquals(block->key(), "connections")) return block;
    }
    return nullptr;
}
}

TEST_CASE("Connection model duplicate outputs retain file order through edits add delete and duplicate") {
    const std::string source = "// untouched\nentity\n{\n id 1\n connections\n {\n"
        "  \"OnTrigger\" \"first,Open,,0.50,-1\" // first\n"
        "  \"OnTrigger\" \"middle\x1b" "Open\x1b\x1b" "0.50\x1b-1\" // middle\n"
        "  \"OnTrigger\" \"last,Open,,0.50,-1\" // last\n }\n}\n";
    Document doc(parsed(source));
    const auto selection = selected(doc);
    CHECK(text(doc) == source);
    ConnectionModel initial(doc, selection);
    REQUIRE(initial.rows().size() == 3);
    CHECK(initial.rows()[1].index == 1);
    auto edit = doc.begin("edit");
    REQUIRE(initial.setField(edit, 1, ConnectionModel::Field::Delay, "2.00"));
    edit.commit(false);
    const auto writtenAfterEdit = text(doc);
    CHECK(writtenAfterEdit.find("\"OnTrigger\" \"first,Open,,0.50,-1\"") != std::string::npos);
    CHECK(writtenAfterEdit.find("\"OnTrigger\" \"middle\x1b" "Open\x1b\x1b" "2.00\x1b-1\"") != std::string::npos);
    CHECK(writtenAfterEdit.find("\"OnTrigger\" \"last,Open,,0.50,-1\"") != std::string::npos);
    auto erase = doc.begin("erase");
    REQUIRE(ConnectionModel(doc, selection).remove(erase, 0));
    erase.commit(false);
    auto rows = ConnectionModel(doc, selection).rows();
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].connection.target == "middle");
    CHECK(rows[0].connection.delay == "2.00");
    CHECK(rows[1].connection.target == "last");
    auto add = doc.begin("add");
    REQUIRE(ConnectionModel(doc, selection).add(add, "OnTrigger", document::parseConnection("new,Open,,0,-1")));
    add.commit(false);
    auto duplicate = doc.begin("duplicate");
    REQUIRE(ConnectionModel(doc, selection).duplicate(duplicate, 0));
    duplicate.commit(false);
    rows = ConnectionModel(doc, selection).rows();
    REQUIRE(rows.size() == 4);
    CHECK(rows[0].connection.raw == rows[1].connection.raw);
    CHECK(rows[2].connection.target == "last");
    CHECK(rows[3].connection.target == "new");
    auto rename = doc.begin("output");
    REQUIRE(ConnectionModel(doc, selection).setOutput(rename, 1, "OnStart"));
    rename.commit(false);
    rows = ConnectionModel(doc, selection).rows();
    CHECK(rows[0].output == "OnTrigger");
    CHECK(rows[1].output == "OnStart");
    CHECK(rows[2].output == "OnTrigger");
    for (int i = 0; i < 5; ++i) REQUIRE(doc.undo());
    CHECK(text(doc) == source);
}

TEST_CASE("Connection model multi selection intersects fields once and preserves each separator") {
    Document doc(parsed("entity { connections { OnTrigger \"unique,Open,,0,-1\" OnTrigger \"shared,Open,,0.50,-1\" OnTrigger \"shared,Open,,0.50,-1\" } }\n"
                        "entity { connections { OnTrigger \"shared\x1b" "Open\x1b\x1b" "0.50\x1b-1\" Other \"shared,Open,,0.50,-1\" } }"));
    auto selection = selected(doc);
    ConnectionModel model(doc, selection);
    REQUIRE(model.rows().size() == 1);
    CHECK(model.rows()[0].indices == std::vector<std::size_t>{1, 0});
    const auto before = text(doc);
    auto tx = doc.begin("edit common");
    REQUIRE(model.setField(tx, 0, ConnectionModel::Field::Parameter, "parameter"));
    tx.commit(false);
    const auto* a = connections(doc, doc.roots()[0]);
    const auto* b = connections(doc, doc.roots()[1]);
    REQUIRE(a);
    REQUIRE(b);
    CHECK(a->children()[1].decodedValue() == "shared,Open,parameter,0.50,-1");
    CHECK(b->children()[0].decodedValue() == "shared\x1b" "Open\x1bparameter\x1b" "0.50\x1b-1");
    CHECK(a->children()[2].decodedValue() == "shared,Open,,0.50,-1");
    REQUIRE(doc.undo());
    CHECK(text(doc) == before);
    selection.add(Target{id(doc, addBlock(doc, "entity {}"))});
    CHECK(ConnectionModel(doc, selection).rows().empty());
    auto add = doc.begin("add to all");
    document::Connection fresh;
    fresh.target = "new";
    REQUIRE(ConnectionModel(doc, selection).add(add, "OnNew", fresh));
    add.commit(false);
    for (const auto entity : doc.roots()) {
        const auto* block = connections(doc, entity);
        REQUIRE(block);
        CHECK(block->children().back().key() == "OnNew");
    }
}

TEST_CASE("Connection model common rows rename duplicate and remove their matching entries") {
    Document doc(parsed("entity { connections { Only \"a,b,c,d,e\" Shared \"t,i,p,d,n\" } } "
                        "entity { connections { Shared \"t,i,p,d,n\" Only \"z,b,c,d,e\" } }"));
    const auto selection = selected(doc);
    const auto before = text(doc);
    auto rename = doc.begin("rename");
    REQUIRE(ConnectionModel(doc, selection).setOutput(rename, 0, "Renamed"));
    rename.commit(false);
    auto duplicate = doc.begin("duplicate");
    REQUIRE(ConnectionModel(doc, selection).duplicate(duplicate, 0));
    duplicate.commit(false);
    CHECK(connections(doc, doc.roots()[0])->children()[1].key() == "Renamed");
    CHECK(connections(doc, doc.roots()[0])->children()[2].key() == "Renamed");
    CHECK(connections(doc, doc.roots()[1])->children()[0].key() == "Renamed");
    CHECK(connections(doc, doc.roots()[1])->children()[1].key() == "Renamed");
    auto remove = doc.begin("remove");
    REQUIRE(ConnectionModel(doc, selection).remove(remove, 0));
    remove.commit(false);
    CHECK(connections(doc, doc.roots()[0])->children()[0].key() == "Only");
    CHECK(connections(doc, doc.roots()[0])->children()[1].key() == "Renamed");
    CHECK(connections(doc, doc.roots()[1])->children()[0].key() == "Renamed");
    CHECK(connections(doc, doc.roots()[1])->children()[1].key() == "Only");
    for (int i = 0; i < 3; ++i) REQUIRE(doc.undo());
    CHECK(text(doc) == before);
}

TEST_CASE("Connection session operations are individual undo steps and last removal drops block") {
    Document doc(parsed("entity { id 1 classname logic_relay editor { color \"1 2 3\" } }\n"));
    EditorSession session;
    session.attach(doc);
    session.selection() = selected(doc);
    const auto original = text(doc);
    const auto check = [&](const auto& operation) {
        const auto before = text(doc);
        const auto count = doc.undoCount();
        REQUIRE(operation());
        CHECK(doc.undoCount() == count + 1);
        REQUIRE(doc.undo());
        CHECK(text(doc) == before);
        REQUIRE(doc.redo());
    };
    document::Connection fresh;
    fresh.target = "door";
    fresh.input = "Open";
    fresh.delay = "0.50";
    fresh.times = "-1";
    check([&] { return session.addConnection("OnTrigger", fresh); });
    REQUIRE(session.connectionModel().rows().size() == 1);
    CHECK(session.connectionModel().rows()[0].connection.separator == '\x1b');
    const auto count = doc.undoCount();
    REQUIRE(session.setConnectionField(0, ConnectionModel::Field::Delay, "0.50"));
    REQUIRE(session.setConnectionOutput(0, "OnTrigger"));
    CHECK(doc.undoCount() == count);
    check([&] { return session.setConnectionField(0, ConnectionModel::Field::Delay, "1"); });
    check([&] { return session.setConnectionField(0, ConnectionModel::Field::Delay, "2"); });
    check([&] { return session.setConnectionOutput(0, "OnStart"); });
    check([&] { return session.duplicateConnection(0); });
    check([&] { return session.removeConnection(0); });
    check([&] { return session.removeConnection(0); });
    CHECK_FALSE(connections(doc, doc.roots()[0]));
    while (doc.undoCount()) REQUIRE(doc.undo());
    CHECK(text(doc) == original);
    auto tx = doc.begin("rollback");
    CHECK_FALSE(session.connectionModel().remove(tx, 10));
}

TEST_CASE("Connection transaction rejects non scalar replacement and rolls back block creation") {
    Document doc(parsed("entity { id 1 }"));
    const auto before = text(doc);
    {
        auto tx = doc.begin("rollback");
        auto invalid = kv1::Node::block("connections");
        invalid.append(kv1::Node::block("nested")).value();
        CHECK_THROWS_AS(tx.setConnections(*doc.handle(doc.roots()[0]), invalid), std::invalid_argument);
        auto valid = kv1::Node::block("connections");
        valid.append(kv1::Node::value("OnTrigger", "a,b,c,d,e")).value();
        tx.setConnections(*doc.handle(doc.roots()[0]), valid);
    }
    CHECK(text(doc) == before);
    CHECK(doc.undoCount() == 0);
}

TEST_CASE("Connection P2CE corpus codec and document preserve every connections block") {
    namespace fs = std::filesystem;
    const char* install = std::getenv("APEX_P2CE_DIR");
    if (!install || !*install) {
        MESSAGE("APEX_P2CE_DIR is unset; skipping Connection corpus comparison");
        return;
    }
    const fs::path root = fs::path(install) / "sdk_content/maps";
    REQUIRE(fs::is_directory(root));
    std::vector<fs::path> paths;
    for (const auto& entry : fs::recursive_directory_iterator(root))
        if (entry.is_regular_file() && entry.path().extension() == ".vmf") paths.push_back(entry.path());
    std::ranges::sort(paths);
    REQUIRE_FALSE(paths.empty());
    std::size_t esc = 0, comma = 0, wellFormed = 0, malformed = 0, blocks = 0;
    for (const auto& path : paths) {
        CAPTURE(path.string());
        std::ifstream stream(path, std::ios::binary);
        REQUIRE(stream.good());
        const std::string bytes{std::istreambuf_iterator<char>(stream), {}};
        auto file = vmf::parse(bytes);
        if (!file) file = vmf::parse(bytes, {kv1::EscapeMode::Legacy});
        REQUIRE(file);
        Document doc(*file);
        const auto written = text(doc);
        CHECK(written == bytes);
        const auto walk = [&](const auto& self, const kv1::Node& node, bool entity) -> void {
            if (entity && ciEquals(node.key(), "connections") && node.kind() == kv1::NodeKind::Block) {
                ++blocks;
                const auto span = node.sourceSpan();
                CHECK(written.substr(span.offset, span.length) == bytes.substr(span.offset, span.length));
                for (const auto& entry : node.children()) {
                    if (entry.kind() != kv1::NodeKind::Value) continue;
                    const auto value = entry.decodedValue();
                    REQUIRE(value);
                    const auto connection = document::parseConnection(*value);
                    CHECK(document::serialize(connection) == *value);
                    (connection.separator == '\x1b' ? esc : comma)++;
                    (connection.wellFormed ? wellFormed : malformed)++;
                }
            }
            for (const auto& child : node.children()) self(self, child, ciEquals(node.key(), "entity"));
        };
        walk(walk, file->kvDocument().root(), false);
    }
    MESSAGE("Connection corpus: " << paths.size() << " maps, " << blocks << " blocks, ESC=" << esc
            << ", comma=" << comma << ", well-formed=" << wellFormed << ", malformed=" << malformed);
    CHECK(esc > 4000);
}
