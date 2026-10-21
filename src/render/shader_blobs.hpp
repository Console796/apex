#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace apex::render {

// SPIR-V for the built-in shaders in src/render/shaders, embedded at build time (plan 0.1, D14).
// `id` is the file name with '.' replaced by '_' and without the ".glsl" suffix, e.g.
// "brush_vert" for brush.vert.glsl. Empty span when unknown.
std::span<const std::uint32_t> builtinSpirv(std::string_view id);

} // namespace apex::render
