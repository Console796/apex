#pragma once

#include "editor/tool.hpp"

#include <chrono>

namespace apex::editor {

enum class PieSlot { N, NE, E, SE, S, SW, W, NW };
DVec2 slotDirection(PieSlot slot);

struct PieItem {
    ActionId action;
    std::optional<PieSlot> slot;
    std::string label;
};
struct PieMenu {
    std::string title;
    std::vector<PieItem> items;
};
std::vector<PieSlot> layoutSlots(const PieMenu& menu);

struct PieSettings {
    double radius = 100, deadZone = 12, dragThreshold = 7;
    std::chrono::milliseconds tapTimeout{200}, animation{60}, confirmStill{20};
    double confirmOnStopDistance = 0;
};
struct PieResult {
    enum Outcome { Pending, Pick, Cancelled } outcome = Pending;
    std::optional<ActionId> action;
};

class PieGesture {
public:
    PieGesture(PieMenu menu, PieSettings settings, DVec2 centre, KeyChord trigger,
               std::function<bool(const ActionId&)> enabled, std::chrono::milliseconds openTime);
    PieResult input(const InputEvent& event, std::chrono::milliseconds now);
    PieResult tick(std::chrono::milliseconds now);
    PieResult focusLost();
    std::optional<std::size_t> highlighted() const;
    DVec2 centre() const { return centre_; }
    bool triggerHeld() const { return triggerHeld_; }
    const KeyChord& trigger() const { return trigger_; }
    bool clickStyle() const { return clickStyle_; }
    double openProgress(std::chrono::milliseconds now) const;

private:
    PieMenu menu_;
    PieSettings settings_;
    DVec2 centre_, cursor_;
    KeyChord trigger_;
    std::function<bool(const ActionId&)> enabled_;
    std::chrono::milliseconds openTime_, lastMotion_;
    std::vector<PieSlot> slots_;
    bool triggerHeld_ = true, dragging_ = false, clickStyle_ = false;
    PieResult result_;

    PieResult pick(std::optional<std::size_t> item);
    std::optional<std::size_t> aimed() const; // Item in the cursor's direction, enabled or not.
    bool disabledAimed() const { return aimed() && !highlighted(); }
};

} // namespace apex::editor
