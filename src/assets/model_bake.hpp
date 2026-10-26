#pragma once

#include "assets/model_data.hpp"
#include "core/result.hpp"

namespace mdlpp {
struct StudioModel;
namespace MDL { struct MDL; }
namespace VTX { struct VTX; }
namespace VVD { struct VVD; }
}

namespace apex::assets {

// LOD 0, with VVD fixups. Invalid input (including mdlpp exceptions) becomes an error.
Result<ModelData> bakeModel(const mdlpp::MDL::MDL& mdl, const mdlpp::VTX::VTX& vtx,
                           const mdlpp::VVD::VVD& vvd);
// Uses the parsed structs directly; StudioModel::open() need not have been called.
Result<ModelData> bakeModel(const mdlpp::StudioModel& model);

} // namespace apex::assets
