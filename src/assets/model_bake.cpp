#include "assets/model_bake.hpp"

#include <mdlpp/mdlpp.h>

namespace apex::assets {

Result<ModelData> bakeModel(const mdlpp::MDL::MDL&, const mdlpp::VTX::VTX&, const mdlpp::VVD::VVD&) {
    return std::unexpected(Error{ErrorCode::Unsupported, "not implemented"});
}

Result<ModelData> bakeModel(const mdlpp::StudioModel& model) {
    return bakeModel(model.mdl, model.vtx, model.vvd);
}

std::vector<std::uint32_t> modelBodyChoices(const ModelData&, std::uint32_t) {
    return {}; // Implemented with the bake in the next slice.
}

std::vector<ModelDrawRange> modelDrawRanges(const ModelData&, std::uint32_t, std::uint32_t) {
    return {};
}

} // namespace apex::assets
