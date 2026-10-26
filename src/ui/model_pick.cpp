#include "ui/model_pick.hpp"

#include <charconv>

namespace apex::ui {

std::shared_ptr<const editor::PickModel> makePickModel(const assets::ModelData& model) {
    std::vector<DVec3> positions;
    positions.reserve(model.vertices.size());
    for (const auto& vertex : model.vertices) positions.emplace_back(vertex.position);
    std::vector<std::uint32_t> indices;
    std::vector<editor::PickModelBodyPart> parts;
    for (std::size_t part = 0; part < model.bodyParts.size(); ++part) {
        auto& output = parts.emplace_back();
        output.base = model.bodyParts[part].base;
        for (std::size_t choice = 0; choice < model.bodyParts[part].choices.size(); ++choice) {
            const auto first = static_cast<std::uint32_t>(indices.size() / 3);
            for (const auto& mesh : model.submeshes) {
                if (mesh.bodyPart != part || mesh.choice != choice) continue;
                if (mesh.firstIndex > model.indices.size() || mesh.indexCount > model.indices.size() - mesh.firstIndex) continue;
                indices.insert(indices.end(), model.indices.begin() + mesh.firstIndex,
                               model.indices.begin() + mesh.firstIndex + mesh.indexCount / 3 * 3);
            }
            output.choices.push_back({{{first, static_cast<std::uint32_t>(indices.size() / 3) - first}}});
        }
    }
    if (parts.empty()) indices = model.indices;
    return std::make_shared<editor::PickModel>(std::move(positions), std::move(indices), std::move(parts));
}

std::optional<editor::EntityPickShape> ModelPickCache::shape(const kv1::Node& entity) {
    const auto* key = entity.find("model");
    const auto name = key ? key->decodedValue().value_or("") : "";
    if (name.empty() || name.front() == '*') return {};
    auto& entry = entries_[assets::modelPath(name)];
    auto source = entry.source ? entry.source : models_.request(name);
    if (source != entry.source) {
        entry.source = std::move(source);
        entry.pick = entry.source ? makePickModel(*entry.source) : nullptr;
    }
    if (!entry.pick) return {};
    const auto* bodyKey = entity.find("body");
    const auto bodyText = bodyKey ? bodyKey->decodedValue().value_or("") : "";
    std::uint32_t body = 0;
    std::from_chars(bodyText.data(), bodyText.data() + bodyText.size(), body);
    return editor::EntityPickShape{entry.pick, body, {}};
}

bool ModelPickCache::pump() {
    bool changed = false;
    for (auto& [name, entry] : entries_) {
        if (entry.source) continue;
        auto source = models_.request(name);
        if (source == entry.source) continue;
        entry.source = std::move(source);
        entry.pick = entry.source ? makePickModel(*entry.source) : nullptr;
        changed = true;
    }
    return changed;
}

} // namespace apex::ui
