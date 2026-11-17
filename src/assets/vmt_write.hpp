#pragma once

#include "assets/vfs.hpp"
#include "core/kv1.hpp"
#include "core/result.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace apex::assets {

// One change to a VMT's top-level parameters (the keys directly inside the shader block).
// `value` empty removes the key.
struct VmtEdit {
    std::string key;
    std::optional<std::string> value;
};

// Applies `edits` to VMT text and returns the new text. Everything the edits do not touch keeps
// its formatting, comments and order (the kv1 writer is lossless). A key is matched
// case-insensitively and keeps its original spelling when its value changes; a missing key is
// appended at the end of the shader block. Patch-shader blocks (insert/replace) are not edited:
// only the shader block's own parameters. Fails (and returns nothing) when the text is not a VMT
// or a value cannot be written, so a failed edit never produces partial text. Keys must be
// non-empty and without whitespace or quotes; values without control characters.
Result<std::string> editVmt(std::string_view source, std::span<const VmtEdit> edits);

// Writes `edits` into the material's VMT through the VFS's winning source. Only a loose file in
// a directory mount can be written: a VMT served from a VPK or a provider returns Unsupported (the
// compile and the game would not see an edit there either). The text is written to a temporary
// file beside the VMT and renamed over it, so a failed write leaves the original. Returns the
// path written. The caller refreshes the Vfs (Vfs::refresh) only if files were added or removed;
// changing an existing file needs no refresh, because paths are read on open.
Result<std::filesystem::path> writeVmt(const Vfs& vfs, std::string_view material,
                                       std::span<const VmtEdit> edits);

} // namespace apex::assets
