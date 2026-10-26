#include "editor/preview.hpp"

namespace apex::editor {
namespace {

bool equal(const geometry::ConvexPolyhedron& a, const geometry::ConvexPolyhedron& b) {
    if (a.vertices != b.vertices || a.faces.size() != b.faces.size()) return false;
    for (std::size_t i = 0; i < a.faces.size(); ++i) {
        const auto& x = a.faces[i];
        const auto& y = b.faces[i];
        if (x.vertices != y.vertices || x.plane.normal != y.plane.normal || x.plane.dist != y.plane.dist ||
            x.sideId != y.sideId || x.material != y.material || x.lightmapScale != y.lightmapScale ||
            x.smoothingGroups != y.smoothingGroups || x.uv.uaxis != y.uv.uaxis || x.uv.vaxis != y.uv.vaxis ||
            x.uv.shiftU != y.uv.shiftU || x.uv.shiftV != y.uv.shiftV ||
            x.uv.scaleU != y.uv.scaleU || x.uv.scaleV != y.uv.scaleV) return false;
    }
    return true;
}

bool equal(const Preview::Replacements& a, const Preview::Replacements& b) {
    if (a.size() != b.size()) return false;
    auto y = b.begin();
    for (const auto& [id, poly] : a) {
        if (id != y->first || !equal(poly, y->second)) return false;
        ++y;
    }
    return true;
}

bool equal(const std::vector<GhostSolid>& a, const std::vector<GhostSolid>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].material != b[i].material || a[i].opacity != b[i].opacity ||
            a[i].outline != b[i].outline || !equal(a[i].poly, b[i].poly)) return false;
    return true;
}

} // namespace

Preview& Preview::operator=(const Preview& other) {
    if (transforms_ == other.transforms_ && equal(replacements_, other.replacements_) && equal(ghosts_, other.ghosts_))
        return *this;
    transforms_ = other.transforms_;
    replacements_ = other.replacements_;
    ghosts_ = other.ghosts_;
    ++revision_;
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

void Preview::setReplacement(NodeId node, const geometry::ConvexPolyhedron& poly) {
    const auto found = replacements_.find(node);
    if (found != replacements_.end() && equal(found->second, poly)) return;
    replacements_[node] = poly;
    ++revision_;
}

void Preview::setReplacements(const Replacements& replacements) {
    if (equal(replacements_, replacements)) return;
    replacements_ = replacements;
    ++revision_;
}

void Preview::setGhosts(const std::vector<GhostSolid>& ghosts) {
    if (equal(ghosts_, ghosts)) return;
    ghosts_ = ghosts;
    ++revision_;
}

void Preview::clear(NodeId node) {
    const auto count = transforms_.erase(node) + replacements_.erase(node);
    if (count) ++revision_;
}

void Preview::clear() {
    if (empty()) return;
    transforms_.clear();
    replacements_.clear();
    ghosts_.clear();
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
