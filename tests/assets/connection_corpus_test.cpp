#include "assets/fgd.hpp"
#include "editor/connection_validation.hpp"
#include "../editor/fixtures.hpp"

#include <array>
#include <cstdlib>
#include <fstream>

using namespace editor_test;
namespace {
std::string escaped(std::string_view value) {
    std::string result;
    for (char c : value) {
        if (c == '\x1b') result += "\\x1b";
        else if (c == '\t') result += "\\t";
        else if (c == '\n') result += "\\n";
        else if (c == '\r') result += "\\r";
        else result += c;
    }
    return result;
}
}
TEST_CASE("Connection validation P2CE corpus reports every issue") {
    const auto* install = std::getenv("APEX_P2CE_DIR");
    if (!install || !*install) { MESSAGE("APEX_P2CE_DIR unset; skipping Connection validation corpus"); return; }
    auto classes = assets::loadEntityClasses(std::filesystem::path(install) / "p2ce/p2ce.fgd");
    REQUIRE_MESSAGE(classes, (classes ? "" : classes.error().what()));
    std::ofstream report;
    if (const auto* path = std::getenv("APEX_CONNECTION_REPORT"); path && *path) {
        report.open(path);
        REQUIRE_MESSAGE(report.good(), path);
        report << "map\tentity id\tsource classname\toutput\tvalue\tissue kind\tmessage\n";
    }
    const std::array<const char*, 7> kinds{"UnknownOutput", "UnmatchedTarget", "UnknownInput", "BadParameter", "BadDelay", "BadTimes", "Malformed"};
    std::array<std::size_t, 7> counts{};
    std::vector<std::filesystem::path> paths;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(std::filesystem::path(install) / "sdk_content/maps"))
        if (entry.is_regular_file() && ciEquals(entry.path().extension().string(), ".vmf")) paths.push_back(entry.path());
    std::ranges::sort(paths);
    REQUIRE_FALSE(paths.empty());
    InstanceTemplates templates;
    std::size_t connections = 0;
    bool activator = false, wildcard = false, instance = false, unresolvedInstance = false;
    for (const auto& path : paths) {
        CAPTURE(path.string());
        std::ifstream stream(path, std::ios::binary);
        REQUIRE(stream.good());
        const std::string bytes{std::istreambuf_iterator<char>(stream), {}};
        auto file = vmf::parse(bytes);
        if (!file) file = vmf::parse(bytes, {kv1::EscapeMode::Legacy});
        REQUIRE(file);
        Document doc(*file);
        NameIndex names(doc);
        ConnectionValidator validator(doc, names, *classes, &templates, path);
        for (const auto h : nodes(doc)) {
            if (!std::holds_alternative<Handle<Entity>>(h)) continue;
            const auto* entity = doc.get(h);
            const auto* cls = entity->block->find("classname");
            const auto classname = cls ? cls->decodedValue().value_or("") : "";
            const auto* idField = entity->block->find("id");
            const auto entityId = idField ? idField->decodedValue().value_or("") : std::to_string(entity->id);
            for (auto child : entity->children) {
                const auto* block = doc.get(*doc.handle(child))->block.get();
                if (!ciEquals(block->key(), "connections")) continue;
                for (const auto& entry : block->children()) {
                    if (entry.kind() != kv1::NodeKind::Value) continue;
                    ++connections;
                    const auto value = entry.decodedValue().value_or("");
                    const auto c = document::parseConnection(value);
                    const auto issues = validator.validate(entity->id, c, entry.key());
                    for (const auto& issue : issues) {
                        const auto index = static_cast<std::size_t>(issue.kind);
                        ++counts[index];
                        if (report.is_open()) report << escaped(path.string()) << '\t' << escaped(entityId) << '\t' << escaped(classname)
                            << '\t' << escaped(entry.key()) << '\t' << escaped(value) << '\t' << kinds[index] << '\t' << escaped(issue.message) << '\n';
                    }
                    const auto has = [&](ConnectionIssueKind kind) {
                        return std::ranges::any_of(issues, [&](const auto& issue) { return issue.kind == kind; });
                    };
                    // Assert one independently recognizable construct of each kind when available.
                    if (!activator && ciEquals(c.target, "!activator") && c.wellFormed) {
                        CHECK_FALSE(has(ConnectionIssueKind::UnmatchedTarget));
                        CHECK_FALSE(has(ConnectionIssueKind::UnknownInput));
                        activator = true;
                    }
                    if (!wildcard && c.target.find('*') != std::string::npos && !validator.matching(c.target).empty()) {
                        CHECK_FALSE(has(ConnectionIssueKind::UnmatchedTarget));
                        wildcard = true;
                    }
                    if (c.wellFormed && c.input.starts_with("instance:")) {
                        const auto semicolon = c.input.find(';');
                        for (auto id : validator.matching(c.target)) {
                            auto proxies = validator.instanceProxies(id);
                            if (!proxies && validator.isInstance(id) && !unresolvedInstance) {
                                CHECK_FALSE(has(ConnectionIssueKind::UnknownInput));
                                unresolvedInstance = true;
                            }
                            if (!proxies || semicolon == std::string::npos || instance) continue;
                            if (!has(ConnectionIssueKind::UnknownInput)) {
                                CHECK_FALSE(has(ConnectionIssueKind::UnknownInput));
                                instance = true;
                            }
                        }
                    }
                }
            }
        }
    }
    if (report.is_open()) { report.flush(); CHECK(report.good()); }
    MESSAGE("Connection validation corpus: maps=" << paths.size() << " connections=" << connections
        << " known-good activator=" << activator << " wildcard=" << wildcard
        << " resolved-instance=" << instance << " unresolved-instance=" << unresolvedInstance);
    for (std::size_t i = 0; i < counts.size(); ++i) MESSAGE(std::string(kinds[i]) << "=" << counts[i]);
    CHECK(connections > 0);
}
