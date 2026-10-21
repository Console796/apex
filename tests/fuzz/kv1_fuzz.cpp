#include "core/kv1.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace {

void require(bool condition) {
    if (!condition) {
        std::abort();
    }
}

void mutate(apex::kv1::Node& parent, std::size_t depth) {
    if (depth > 256) {
        return;
    }
    std::size_t index = 0;
    for (auto& child : parent.children()) {
        if (child.kind() == apex::kv1::NodeKind::Block) {
            mutate(child, depth + 1);
        } else {
            static_cast<void>(child.asInt());
            static_cast<void>(child.asDouble());
            static_cast<void>(child.asVec3());
            static_cast<void>(child.asColor255());
            if (index++ % 3 == 0) {
                require(child.setValue("changed\\n\tvalue").has_value());
                require(child.setCondition("[!$FUZZ]").has_value());
            }
        }
    }
    if (depth % 2 == 0) {
        require(parent.insert(0, apex::kv1::Node::value("fuzz", "42")).has_value());
        require(parent.append(apex::kv1::Node::block("new_block")).has_value());
        require(parent.remove(parent.children().size() - 1).has_value());
    }
}

void equivalent(const apex::kv1::Node& before, const apex::kv1::Node& after) {
    require(before.kind() == after.kind());
    require(before.key() == after.key());
    require(before.condition() == after.condition());
    require(before.children().size() == after.children().size());
    if (before.kind() != apex::kv1::NodeKind::Block) {
        const auto left = before.decodedValue();
        const auto right = after.decodedValue();
        require(left && right && *left == *right);
    }
    for (std::size_t i = 0; i < before.children().size(); ++i) {
        equivalent(before.children()[i], after.children()[i]);
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string source(reinterpret_cast<const char*>(data), size);
    for (const auto mode : {apex::kv1::EscapeMode::Strata, apex::kv1::EscapeMode::Legacy}) {
        auto parsed = apex::kv1::parse(source, mode);
        if (!parsed) {
            continue;
        }
        const auto written = apex::kv1::write(*parsed);
        require(written && *written == source);
        mutate(parsed->root(), 0);
        const auto changed = apex::kv1::write(*parsed);
        require(changed.has_value());
        const auto reparsed = apex::kv1::parse(*changed, mode);
        require(reparsed.has_value());
        equivalent(parsed->root(), reparsed->root());
        const auto stable = apex::kv1::write(*reparsed);
        require(stable && *stable == *changed);
    }
    return 0;
}
