#include "editor/preview.hpp"

namespace apex::editor {

Preview& Preview::operator=(const Preview& other) {
    set(other.transforms_);
    return *this;
}

void Preview::set(NodeId node, const Transform& transform) {
    const auto matrix = transform.matrix();
    const auto found = transforms_.find(node);
    if (found != transforms_.end() && found->second == matrix) return;
    transforms_[node] = matrix;
    ++revision_;
}

void Preview::set(const Transforms& transforms) {
    if (transforms_ == transforms) return;
    transforms_ = transforms;
    ++revision_;
}

void Preview::clear(NodeId node) {
    if (transforms_.erase(node)) ++revision_;
}

void Preview::clear() {
    if (empty()) return;
    transforms_.clear();
    ++revision_;
}

Preview fromTransform(std::span<const NodeId> nodes, const Transform& transform) {
    Preview::Transforms transforms;
    for (const auto node : nodes) transforms.emplace(node, transform.matrix());
    Preview preview;
    preview.set(transforms);
    return preview;
}

} // namespace apex::editor
