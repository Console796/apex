#include "corpus.hpp"
#include "editor/editor_session.hpp"
#include "editor/group_ops.hpp"
#include "editor/transform.hpp"

#include <chrono>
#include <iostream>

using namespace editor_test;

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t undoCap = 512ULL * 1024 * 1024;

template <typename F> double milliseconds(F&& operation) {
    const auto start = Clock::now();
    operation();
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

template <typename F> double median(F&& operation) {
    std::vector<double> samples;
    for (int i = 0; i < 7; ++i) samples.push_back(milliseconds(operation));
    std::ranges::sort(samples);
    return samples[samples.size() / 2];
}

double mib(std::size_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); }

// Loaded synthetic six-sided grid, including ordinary Hammer side/editor metadata.
// Parsing the seed removes creation history from the measured move/session.
vmf::VmfFile synthetic(std::size_t count) {
    Document seed(parsed());
    auto tx = seed.begin("seed");
    const auto world = tx.create(block("world { \"id\" \"1\" \"classname\" \"worldspawn\" }"));
    for (std::size_t i = 0; i < count; ++i) {
        const DVec3 min{static_cast<double>(i % 100) * 80.0, static_cast<double>(i / 100) * 80.0, 0};
        auto solid = solidBlock(geometry_test::build(geometry_test::boxSides(min, min + DVec3{64.0})));
        for (auto* side : solid.findAll("side")) {
            side->append(kv1::Node::value("material", "TOOLS/TOOLSNODRAW")).value();
            side->append(kv1::Node::value("uaxis", "[1 0 0 0] 0.25")).value();
            side->append(kv1::Node::value("vaxis", "[0 -1 0 0] 0.25")).value();
            side->append(kv1::Node::value("rotation", "0")).value();
            side->append(kv1::Node::value("lightmapscale", "16")).value();
            side->append(kv1::Node::value("smoothing_groups", "0")).value();
        }
        solid.append(block("editor { \"color\" \"0 190 80\" \"visgroupshown\" \"1\" \"visgroupautoshown\" \"1\" }")).value();
        tx.create(std::move(solid), world);
    }
    tx.create(block("entity { \"classname\" \"info_target\" \"targetname\" \"measure\" \"origin\" \"0 0 0\" }"));
    tx.commit(false);
    return parsed(seed.write().value());
}

std::vector<NodeId> solidIds(const Document& doc) {
    std::vector<NodeId> result;
    for (const auto h : nodes(doc)) {
        if (!std::holds_alternative<Handle<Solid>>(h)) continue;
        const auto* node = doc.get(h);
        const auto geometry = geometry::solidGeometry(*node->block);
        if (geometry && geometry->sides.size() == geometry->poly.faces.size()) result.push_back(node->id);
    }
    return result;
}

void select(EditorSession& session, std::span<const NodeId> ids) {
    std::vector<Target> targets;
    for (const auto node : ids) targets.emplace_back(node);
    session.selection().replace(targets);
}

} // namespace

TEST_CASE("[measure] 0.2 release undo memory and frame costs") {
    const char* enabled = std::getenv("APEX_MEASURE");
    if (!enabled || std::string_view(enabled) != "1") {
        std::cout << "APEX_MEASURE=1 is required; skipping measurements\n";
        return;
    }
    std::cout << std::fixed << std::setprecision(3)
              << "0.2 release measurements (steady_clock; memory = Document::historyBytes, MiB)\n"
              << "workload | ms | history MiB | retained undo entries\n";
    {
        Document doc(synthetic(10000));
        EditorSession session;
        session.attach(doc);
        session.selection().setMode(SelectionMode::Solids);
        session.selection().setIgnoreGroups(true);
        session.setTextureLock(false);
        const auto solids = solidIds(doc);
        REQUIRE(solids.size() == 10000);
        select(session, solids);
        const double elapsed = milliseconds([&] { REQUIRE(session.moveSelection({16, 0, 0}, true)); });
        REQUIRE(doc.undoCount() == 1);
        std::cout << "10000-solid move | " << elapsed << " | " << mib(doc.historyBytes()) << " | " << doc.undoCount() << '\n';
    }
    {
        Document doc(synthetic(256));
        EditorSession session;
        session.attach(doc);
        session.selection().setMode(SelectionMode::Objects);
        session.selection().setIgnoreGroups(true);
        session.setTextureLock(false);
        const auto solids = solidIds(doc);
        NodeId entity = 0;
        for (const auto h : nodes(doc))
            if (doc.get(h)->block->key() == "entity") entity = doc.get(h)->id;
        REQUIRE(entity);
        std::size_t peak = 0;
        // 300 cycles x six API edits = 1800 edits, one per simulated second (no idle).
        // Each cycle: move 32, rotate 16, duplicate one, delete the duplicate, type two
        // targetname values in one focus. The typing pair must coalesce: 1500 entries
        // before cap eviction. This is a workload model, not a real 30-minute user session.
        const double elapsed = milliseconds([&] {
            for (std::size_t cycle = 0; cycle < 300; ++cycle) {
                std::vector<NodeId> batch;
                for (std::size_t i = 0; i < 32; ++i) batch.push_back(solids[(cycle * 17 + i) % solids.size()]);
                select(session, batch);
                REQUIRE(session.moveSelection({cycle % 2 ? -16.0 : 16.0, 0, 0}, true));
                select(session, std::span{batch}.first(16));
                REQUIRE(session.rotateSelection({0, 0, cycle % 2 ? -15.0 : 15.0}));
                session.selection().select(batch[0]);
                REQUIRE(session.duplicateSelection());
                REQUIRE(session.deleteSelection());
                session.selection().select(entity);
                const auto model = session.inspectorModel();
                const auto row = std::ranges::find(model.rows(), "targetname", &InspectorModel::Row::key);
                REQUIRE(row != model.rows().end());
                const auto index = static_cast<std::size_t>(row - model.rows().begin());
                const std::string focus = "measure-focus-" + std::to_string(cycle);
                REQUIRE(session.editProperty(index, "measure_" + std::to_string(cycle), focus));
                const auto history = doc.undoCount();
                REQUIRE(session.editProperty(index, "measure_" + std::to_string(cycle) + "_done", focus));
                REQUIRE(doc.undoCount() == history);
                peak = std::max(peak, doc.historyBytes());
                if ((cycle + 1) % 50 == 0)
                    std::cout << "session edit " << (cycle + 1) * 6 << " | - | " << mib(doc.historyBytes())
                              << " | " << doc.undoCount() << '\n';
            }
        });
        CHECK(doc.historyBytes() <= undoCap);
        std::cout << "session total / peak | " << elapsed << " | " << mib(peak) << " | " << doc.undoCount()
                  << " (1500 pre-eviction entries)\n";
    }

    const auto paths = corpusPaths();
    if (paths.empty()) {
        std::cout << "APEX_P2CE_DIR is unset; corpus frame/write measurements unavailable\n";
        return;
    }
    // Largest by file bytes; sorted path breaks ties deterministically.
    const auto largest = *std::ranges::max_element(paths, {}, [](const auto& p) { return std::filesystem::file_size(p); });
    Document doc(readCorpus(largest));
    std::cout << "largest corpus: " << largest << " (" << std::filesystem::file_size(largest) << " bytes)\n";
    std::unique_ptr<PickIndex> picks;
    const double build = milliseconds([&] { picks = std::make_unique<PickIndex>(doc); });
    std::cout << "pick initial build | " << build << " | - | - (" << picks->size() << " visible items)\n";
    auto solids = solidIds(doc);
    REQUIRE_FALSE(solids.empty());
    REQUIRE(picks->bounds(solids.front())); // The single-solid sample must refit a visible leaf.
    ChangeSet changes;
    const auto observer = doc.observe([&](const ChangeSet& event) { changes = event; });
    REQUIRE(commitTransform(doc, solids, Transform::translate({16, 0, 0}), {.textureLock = false}, "Move large"));
    const double large = milliseconds([&] { picks->apply(changes); });
    std::cout << "pick apply large edit (" << solids.size() << " solids; may rebuild) | " << large << " | - | -\n";
    // Median of seven independent one-solid edits, applying the real emitted ChangeSet.
    std::vector<double> refits;
    for (int i = 0; i < 7; ++i) {
        REQUIRE(commitTransform(doc, std::span{solids}.first(1), Transform::translate({1, 0, 0}), {.textureLock = false}, "Move one"));
        refits.push_back(milliseconds([&] { picks->apply(changes); }));
    }
    std::ranges::sort(refits);
    std::cout << "pick single-solid refit (median 7) | " << refits[3] << " | - | -\n";
    doc.unobserve(observer);

    const auto handles = nodes(doc);
    std::vector<NodeId> ids;
    for (const auto h : handles) ids.push_back(doc.get(h)->id);
    std::uint64_t checksum = 0;
    const double resolution = median([&] {
        for (std::size_t i = 0; i < 100000; ++i) {
            const auto h = doc.handle(ids[(i * 8191) % ids.size()]);
            if (h) checksum += h->index() + 1;
        }
    });
    const double arena = median([&] {
        for (std::size_t i = 0; i < 100000; ++i) checksum += doc.get(handles[(i * 8191) % handles.size()])->id;
    });
    CHECK(checksum > 0); // Consume results so optimized builds retain the lookups.
    std::cout << "doc.handle 100k (median 7) | " << resolution << " | - | - (" << resolution * 10.0 << " ns/lookup)\n"
              << "doc.get cached handles 100k (median 7) | " << arena << " | - | -\n";
    std::size_t written = 0;
    const double write = median([&] {
        const auto bytes = doc.write();
        REQUIRE(bytes);
        written = bytes->size();
    });
    CHECK(written > 0);
    std::cout << "Document::write edited largest (median 7) | " << write << " | - | - (" << written << " output bytes)\n";
}
