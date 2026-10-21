#pragma once

#include <string>
#include <string_view>

namespace apex {

// Source virtual paths: both separators become '/', repeated separators collapse, and dot
// components resolve lexically. Leading '/' is the root (never traversed above); relative
// leading '..' survives. Relative paths reduced to nothing become empty. Root stays '/'; other
// trailing separators disappear. No filesystem access, drive-letter or UNC interpretation.
std::string normalizePath(std::string_view path);
std::string lookupKey(std::string_view path);

// Operate on the normalized final component. Extension includes '.', dotfiles have no
// extension, and a final '.' is an extension. Root and empty paths have an empty stem.
std::string extension(std::string_view path);
std::string stem(std::string_view path);

// An absolute right-hand path replaces the left-hand path; otherwise concatenate and normalize.
std::string join(std::string_view left, std::string_view right);

} // namespace apex
