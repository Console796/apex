#pragma once

#include "assets/model_data.hpp"
#include "core/result.hpp"

#include <cstddef>
#include <span>

namespace mdlpp {
struct StudioModel;
namespace MDL { struct MDL; }
namespace VTX { struct VTX; }
namespace VVD { struct VVD; }
}

namespace apex::assets {

// LOD 0, with VVD fixups. Invalid input (including mdlpp exceptions) becomes an error.
// With `mdlBytes` (the raw .mdl) the vertices are posed as sequence 0, frame 0, which is what Hammer and the
// engine show; without them they stay in reference space.
Result<ModelData> bakeModel(const mdlpp::MDL::MDL& mdl, const mdlpp::VTX::VTX& vtx, const mdlpp::VVD::VVD& vvd,
                            std::span<const std::byte> mdlBytes = {});
// Uses the parsed structs directly; StudioModel::open() need not have been called.
Result<ModelData> bakeModel(const mdlpp::StudioModel& model, std::span<const std::byte> mdlBytes = {});

} // namespace apex::assets
