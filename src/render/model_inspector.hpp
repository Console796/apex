#pragma once

#include "render/model_thumbnail_scheduler.hpp"

#include <cstdint>
#include <optional>

namespace apex::render {

struct ModelInspectBodyPart {
    std::string name;
    std::uint32_t base = 1; // The part's choice is (body / base) % choices.size().
    std::vector<std::string> choices;
};
struct ModelInspectMaterial {
    std::string name;           // The resolved "dir/name" the cache is asked for, else the model's own name.
    bool resolved = false;      // A VMT was found in one of the model's material directories.
    assets::MaterialState state = assets::MaterialState::Unloaded;
    std::size_t triangles = 0;  // Drawn with this material in the current body and skin.
};
// What the inspector shows next to the image. Counts cover the draw ranges of the current body and skin.
struct ModelInspectStats {
    std::size_t vertices = 0, triangles = 0, submeshes = 0;
    DVec3 size{0.0};
    std::size_t skinFamilies = 0;
    std::vector<ModelInspectBodyPart> bodyParts;
    std::vector<ModelInspectMaterial> materials;
};
struct ModelInspection {
    enum class State { Idle, Loading, Ready, Failed } state = State::Idle;
    std::optional<assets::Thumbnail> image; // The last render of this path; null until the first.
    ModelInspectStats stats;
    std::optional<Error> error;
    std::uint64_t revision = 0; // Changes whenever anything above does.
};

// The model inspector's loader and renderer, one model at a time. Owner thread, with the same
// dependency rules as ModelThumbnailScheduler. `update` is polled: it starts the model and the
// current skin's material loads, and renders (not during an interaction) whenever the path, view or
// the set of settled materials changed since the last render, so geometry shows before textures.
class ModelInspector {
public:
    using Render = std::function<Result<assets::Thumbnail>(const assets::ModelData&, const ModelMaterials&,
                                                           const ModelView&)>;
    ModelInspector(assets::ModelCache& models, assets::MaterialCache& materials,
                   std::function<bool(const std::string&)> materialExists, ModelThumbnailRenderer& renderer);
    ModelInspector(ModelThumbnailScheduler::ModelSource models, ModelThumbnailScheduler::MaterialSource materials,
                   Render render);
    const ModelInspection& update(std::string_view path, const ModelView& view, bool interacting);
    void clear();
    const ModelInspection& current() const { return inspection_; }
private:
    void fill(const assets::ModelData& model, const ModelView& view);
    ModelThumbnailScheduler::ModelSource models_;
    ModelThumbnailScheduler::MaterialSource materials_;
    Render render_;
    std::string path_;
    ModelView view_;
    std::size_t settled_ = static_cast<std::size_t>(-1);
    bool rendered_ = false;
    ModelInspection inspection_;
};

} // namespace apex::render
