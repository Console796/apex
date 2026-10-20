#pragma once

#include <string_view>

namespace apex {

// MAJOR.MILESTONE.SUB.BUILD (plan 0.1, D1). BUILD is 0 outside tools/ci.py.
struct Version {
    int major;
    int milestone;
    int sub;
    int build;
    std::string_view string;
    std::string_view gitHash;
    bool gitDirty;
};

const Version& version();

}  // namespace apex
