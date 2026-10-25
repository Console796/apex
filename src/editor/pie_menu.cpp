#include "editor/pie_menu.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace apex::editor {

DVec2 slotDirection(PieSlot slot) {
    const double diagonal = std::sqrt(0.5);
    switch (slot) {
    case PieSlot::N: return {0, -1};
    case PieSlot::NE: return {diagonal, -diagonal};
    case PieSlot::E: return {1, 0};
    case PieSlot::SE: return {diagonal, diagonal};
    case PieSlot::S: return {0, 1};
    case PieSlot::SW: return {-diagonal, diagonal};
    case PieSlot::W: return {-1, 0};
    case PieSlot::NW: return {-diagonal, -diagonal};
    }
    throw std::invalid_argument("Invalid pie slot");
}

std::vector<PieSlot> layoutSlots(const PieMenu& menu) {
    constexpr std::array order{PieSlot::W, PieSlot::E, PieSlot::S, PieSlot::N,
                               PieSlot::NW, PieSlot::NE, PieSlot::SW, PieSlot::SE};
    if (menu.items.size() > order.size()) throw std::invalid_argument("Pie menu has more than eight items");
    std::array<bool, 8> used{};
    for (const auto& item : menu.items) {
        if (!item.slot) continue;
        const auto index = static_cast<std::size_t>(*item.slot);
        if (index >= used.size() || used[index]) throw std::invalid_argument("Invalid or duplicate pie slot");
        used[index] = true;
    }
    std::vector<PieSlot> result;
    for (const auto& item : menu.items) {
        if (item.slot) { result.push_back(*item.slot); continue; }
        const auto free = std::find_if(order.begin(), order.end(), [&](PieSlot slot) {
            return !used[static_cast<std::size_t>(slot)];
        });
        result.push_back(*free);
        used[static_cast<std::size_t>(*free)] = true;
    }
    return result;
}

PieGesture::PieGesture(PieMenu menu, PieSettings settings, DVec2 centre, KeyChord trigger,
                       std::function<bool(const ActionId&)> enabled, std::chrono::milliseconds openTime)
    : menu_(std::move(menu)), settings_(settings), centre_(centre), cursor_(centre),
      trigger_(std::move(trigger)), enabled_(std::move(enabled)), openTime_(openTime),
      lastMotion_(openTime), slots_(layoutSlots(menu_)) {}

std::optional<std::size_t> PieGesture::highlighted() const {
    const auto item = aimed();
    if (item && !enabled_(menu_.items[*item].action)) return {};
    return item;
}

std::optional<std::size_t> PieGesture::aimed() const {
    if (result_.outcome != PieResult::Pending) return {};
    const auto offset = cursor_ - centre_;
    const auto distance = glm::length(offset);
    if (distance <= settings_.deadZone || distance == 0) return {};
    std::optional<std::size_t> nearest;
    double best = -std::numeric_limits<double>::infinity();
    // Bisect occupied directions: eight sectors are 45 degrees, four are 90, two are 180.
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        const double alignment = glm::dot(offset / distance, slotDirection(slots_[i]));
        if (alignment > best) { best = alignment; nearest = i; }
    }
    return nearest;
}

PieResult PieGesture::pick(std::optional<std::size_t> item) {
    if (item && enabled_(menu_.items[*item].action)) result_ = {PieResult::Pick, menu_.items[*item].action};
    else result_ = {PieResult::Cancelled, {}};
    return result_;
}

PieResult PieGesture::focusLost() {
    if (result_.outcome == PieResult::Pending) result_ = {PieResult::Cancelled, {}};
    return result_;
}

PieResult PieGesture::input(const InputEvent& event, std::chrono::milliseconds now) {
    if (result_.outcome != PieResult::Pending) return result_;
    if ((event.kind == InputKind::KeyDown || event.kind == InputKind::KeyUp) && event.autoRepeat) return result_;
    const auto& key = event.chord ? event.chord->key : event.key;
    if (event.kind == InputKind::Cancel ||
        (event.kind == InputKind::KeyDown && key == "Escape") ||
        (event.kind == InputKind::Press && event.button == PointerButton::Right)) return focusLost();

    if (event.kind == InputKind::Move || event.kind == InputKind::Press || event.kind == InputKind::Release) {
        if (event.cursor != cursor_) { cursor_ = event.cursor; lastMotion_ = now; }
        if (triggerHeld_ && glm::length(cursor_ - centre_) >= settings_.dragThreshold) dragging_ = true;
    }
    if (event.kind == InputKind::KeyUp && key == trigger_.key && triggerHeld_) {
        triggerHeld_ = false;
        if (!dragging_ && now - openTime_ < settings_.tapTimeout) clickStyle_ = true;
        else if (!disabledAimed()) return pick(highlighted());
        else clickStyle_ = true; // Released on a disabled item: stay open for a click.
    }
    if (event.kind == InputKind::Press && event.button == PointerButton::Left && clickStyle_ && !disabledAimed())
        return pick(highlighted());
    if (event.kind == InputKind::KeyDown) {
        if (key == trigger_.key) return result_;
        constexpr std::array digits{'8', '9', '6', '3', '2', '1', '4', '7'};
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            if (key.size() == 1 && key[0] == digits[static_cast<std::size_t>(slots_[i])]) {
                if (enabled_(menu_.items[i].action)) return pick(i);
                return result_;
            }
        }
    }
    return result_;
}

PieResult PieGesture::tick(std::chrono::milliseconds now) {
    if (result_.outcome == PieResult::Pending && settings_.confirmOnStopDistance > 0 &&
        glm::length(cursor_ - centre_) > settings_.confirmOnStopDistance &&
        now - lastMotion_ >= settings_.confirmStill) {
        if (const auto item = highlighted()) return pick(item);
    }
    return result_;
}

double PieGesture::openProgress(std::chrono::milliseconds now) const {
    if (settings_.animation.count() <= 0) return 1;
    return std::clamp(static_cast<double>((now - openTime_).count()) /
                      static_cast<double>(settings_.animation.count()), 0.0, 1.0);
}

} // namespace apex::editor
