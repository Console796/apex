#pragma once

#include "assets/addons.hpp"
#include "core/job_pool.hpp"

#include <unordered_map>

namespace apex::assets {

// Directory metadata only. First addon in list order wins; indices refer to the supplied list.
class UnmountedAddonIndex {
public:
    explicit UnmountedAddonIndex(const AddonList& list, CancelToken token = CancelToken{});
    std::optional<std::size_t> provider(std::string_view path) const;
    const std::vector<std::string>& problems() const { return problems_; }
private:
    std::unordered_map<std::string, std::size_t> paths_;
    std::vector<std::string> problems_;
};

} // namespace apex::assets
