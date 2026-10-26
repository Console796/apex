#pragma once

#include "editor/draw_extrude_tool.hpp"
#include "geometry/primitives.hpp"
#include "geometry/solid_block.hpp"

namespace apex::editor {
class PrimitiveTool final : public DrawExtrudeTool {
public:
    using DrawExtrudeTool::DrawExtrudeTool;
    ToolId id() const override { return "tool.primitive"; }
    ToolReadout readout() const override;
    bool setParameter(std::string_view id, double value) override;
    void documentChanged() override;
    void cancel() override;
private:
    geometry::PrimitiveParameters parameters_;
    std::optional<Gesture> lastGesture_;
    std::vector<NodeId> lastNodes_;
    std::string coalesceKey_;
    std::uint64_t operation_ = 0;
    bool writing_ = false;
    std::optional<geometry::NewSolidStyle> lastStyle_;
    std::vector<geometry::ConvexPolyhedron> shapes(const Gesture& gesture) const override;
    bool commitShape(const Gesture& gesture) override;
    void gestureStarted() override;
    void write(const std::vector<geometry::ConvexPolyhedron>& solids);
};
} // namespace apex::editor
