#include "render/shader_blobs.hpp"

#include "render/shaders_spv.hpp"

#include <array>
#include <utility>

namespace apex::render {

namespace {
const std::array<std::pair<std::string_view, std::span<const std::uint32_t>>, 14> kBlobs{{
    {"brush_vert", std::span{shaders::brush_vert}},
    {"brush_textured_frag", std::span{shaders::brush_textured_frag}},
    {"brush_flat_frag", std::span{shaders::brush_flat_frag}},
    {"solid_frag", std::span{shaders::solid_frag}},
    {"box_vert", std::span{shaders::box_vert}},
    {"vcolor_frag", std::span{shaders::vcolor_frag}},
    {"sprite_vert", std::span{shaders::sprite_vert}},
    {"sprite_frag", std::span{shaders::sprite_frag}},
    {"grid_vert", std::span{shaders::grid_vert}},
    {"grid_frag", std::span{shaders::grid_frag}},
    {"overlay_vert", std::span{shaders::overlay_vert}},
    {"label_frag", std::span{shaders::label_frag}},
    {"fullscreen_vert", std::span{shaders::fullscreen_vert}},
    {"tonemap_frag", std::span{shaders::tonemap_frag}},
}};
} // namespace

std::span<const std::uint32_t> builtinSpirv(std::string_view id) {
    for (const auto& [name, blob] : kBlobs) {
        if (name == id) return blob;
    }
    return {};
}

} // namespace apex::render
