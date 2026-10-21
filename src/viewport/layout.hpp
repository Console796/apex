#pragma once

// Split-tree layout of viewport panes (plan 0.1, D15/D16). Pure data model: the UI mirrors it
// into splitters and native child windows. Orientation uses Qt's meaning so the UI maps 1:1:
//   Horizontal = children side by side (first left, second right), Vertical = stacked (first top).

#include "core/json.hpp"
#include "core/result.hpp"
#include "viewport/viewport.hpp"

#include <memory>
#include <optional>
#include <vector>

namespace apex::viewport {

using NodeId = int; // Panes and splits share one id space; ids are never reused in a layout.

enum class Orientation { Horizontal, Vertical };

struct Rect {
    double x = 0, y = 0, w = 1, h = 1;
    bool operator==(const Rect&) const = default;
};

struct PaneRect {
    NodeId pane;
    Rect rect;
};

struct SplitInfo {
    NodeId split;
    Orientation orientation;
    double ratio; // Share of the first child.
    Rect rect;    // The area the split divides.
};

// Read-only snapshot of the split tree for the UI. Leaves are panes; a split has exactly two
// children (first, second) and carries its orientation and ratio.
struct LayoutNode {
    NodeId id = 0;
    bool leaf = true;
    Orientation orientation = Orientation::Horizontal;
    double ratio = 0.5;
    std::vector<LayoutNode> children;
};

class ViewportLayout {
public:
    ViewportLayout();                                // One Camera-preset pane.
    explicit ViewportLayout(Viewport single);
    ViewportLayout(const ViewportLayout&);
    ViewportLayout& operator=(const ViewportLayout&);
    ViewportLayout(ViewportLayout&&) noexcept;
    ViewportLayout& operator=(ViewportLayout&&) noexcept;
    ~ViewportLayout();

    // Camera | Top over Front | Side; pane order (and so view slots v0..v3) is Camera, Top, Front, Side.
    static ViewportLayout quad();

    std::vector<NodeId> paneIds() const; // Depth-first, first child before second.
    std::size_t paneCount() const;
    bool hasPane(NodeId id) const;
    Viewport* viewport(NodeId pane);
    const Viewport* viewport(NodeId pane) const;

    // Splits `pane`; the new pane (returned) is a copy of it and comes second. Clears maximize.
    Result<NodeId> split(NodeId pane, Orientation orientation, double ratio = 0.5);
    // Removes a pane; its sibling takes the parent's place. The last pane cannot be closed.
    Result<void> close(NodeId pane);
    Result<void> setRatio(NodeId split, double ratio); // Clamped to [0.05, 0.95].

    // Maximize shows one pane full-size; the tree is untouched, so restore() is exact.
    Result<void> maximize(NodeId pane);
    void restore() { maximized_.reset(); }
    std::optional<NodeId> maximized() const { return maximized_; }
    void toggleMaximize(NodeId pane);

    NodeId active() const { return active_; }
    Result<void> setActive(NodeId pane);

    // Pane rectangles inside `area` (only the maximized pane while maximized) and splitters.
    std::vector<PaneRect> layout(Rect area = {0, 0, 1, 1}) const;
    std::vector<SplitInfo> splits(Rect area = {0, 0, 1, 1}) const;

    // The whole tree (ignores maximize; the UI decides what to show while maximized).
    LayoutNode tree() const;

    json::Value toJson() const;
    static Result<ViewportLayout> fromJson(const json::Value& value);

private:
    struct Node;
    std::unique_ptr<Node> root_;
    NodeId nextId_ = 1;
    NodeId active_ = 0;
    std::optional<NodeId> maximized_;
};

} // namespace apex::viewport
