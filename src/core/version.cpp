#include "core/version.hpp"

#include <apex/version.hpp>

namespace apex {

const Version& version() {
    static constexpr Version v{APEX_VERSION_MAJOR, APEX_VERSION_MILESTONE, APEX_VERSION_SUB,
                               APEX_VERSION_BUILD, APEX_VERSION_STRING,    APEX_GIT_HASH,
                               APEX_GIT_DIRTY != 0};
    return v;
}

}  // namespace apex
