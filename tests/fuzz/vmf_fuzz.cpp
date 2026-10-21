#include "document/vmf.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace {

void require(bool condition) {
    if (!condition) {
        std::abort();
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto source = std::string_view(reinterpret_cast<const char*>(data), size);
    auto file = apex::vmf::parse(source);
    if (!file) {
        return 0;
    }
    require(apex::vmf::write(*file) == source);
    if (file->world && !file->world->solids.empty()) {
        auto& solid = file->world->solids.front();
        if (auto* id = solid.id()) {
            require(id->set(42).has_value());
            const auto changed = apex::vmf::write(*file);
            auto reparsed = apex::vmf::parse(changed);
            require(reparsed.has_value());
            require(apex::vmf::write(*reparsed) == changed);
        }
    }
    return 0;
}
