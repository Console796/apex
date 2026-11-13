#include "editor/overlay_problems.hpp"

#include "core/strings.hpp"
#include "document/overlay_fragments.hpp"

namespace apex::editor {

void overlayProblems(const ProblemContext& context, std::vector<Problem>& out) {
    const auto& doc = context.doc;
    for (const auto id : document::entityNodes(doc)) {
        const auto* node = doc.get(*doc.handle(id));
        if (!node || !node->block) continue;
        const auto kind = document::overlayKind(*node->block);
        if (!kind || *kind == document::OverlayKind::Decal) continue;
        bool attached = false;
        for (const auto* key : {"sides", "sides2"}) {
            const auto* entry = node->block->find(key);
            if (!entry) continue;
            for (const int side : geometry::parseSideList(entry->decodedValue().value_or("")))
                if (doc.side(side)) attached = true;
        }
        if (attached) continue;
        const auto* classKey = node->block->find("classname");
        const std::string classname = classKey ? classKey->decodedValue().value_or("") : "";
        const auto* nameKey = node->block->find("targetname");
        const std::string name = nameKey ? nameKey->decodedValue().value_or("") : "";
        out.push_back({ProblemSeverity::Warning, ProblemKind::EmptyOverlay,
                       (name.empty() ? classname : classname + " \"" + name + "\"") +
                           " is on no face; VBSP will drop it",
                       {id}, "select"});
    }
}

} // namespace apex::editor
