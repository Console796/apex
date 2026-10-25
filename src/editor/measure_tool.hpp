#pragma once
#include "editor/tool.hpp"

namespace apex::editor {
struct Measurement {
    double distance = 0;
    DVec3 delta{0};
    std::optional<double> faceAngle;
};
Measurement measure(DVec3 a, DVec3 b, std::optional<DVec3> normalA = {}, std::optional<DVec3> normalB = {});
class MeasureTool final : public Tool {
public:
    explicit MeasureTool(EditorSession& session) : session_(session) {}
    ToolId id() const override { return "tool.measure"; }
    Handled input(const InputEvent&, const ToolView&) override;
    const Preview& preview() const override { return preview_; }
    ToolOverlay overlay(const ToolView&, InputSource) const override;
    ToolReadout readout() const override;
    void cancel() override;
private:
    struct Point { DVec3 position; std::optional<DVec3> normal; };
    EditorSession& session_;
    std::vector<Point> points_;
    Preview preview_;
    std::string detail() const;
};
} // namespace apex::editor
