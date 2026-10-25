#include "editor/pie_menus.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <set>
#include <stdexcept>

using namespace apex;
using namespace apex::editor;
using namespace std::chrono_literals;

namespace {
bool enabled(const ActionId&) { return true; }
PieMenu unpinned(std::size_t count) {
    PieMenu menu{"Test", {}};
    for (std::size_t i = 0; i < count; ++i) menu.items.push_back({std::to_string(i), {}, "Item"});
    return menu;
}
InputEvent pointer(DVec2 position, InputKind kind = InputKind::Move,
                   PointerButton button = PointerButton::None) {
    InputEvent event;
    event.kind = kind; event.cursor = position; event.button = button;
    return event;
}
InputEvent key(std::string name, InputKind kind = InputKind::KeyDown, bool chord = false) {
    InputEvent event;
    event.kind = kind;
    if (chord) event.chord = KeyChord{name};
    else event.key = std::move(name);
    return event;
}
PieGesture gesture(PieMenu menu = toolPie(), PieSettings settings = {}) {
    return {std::move(menu), settings, {0, 0}, {"Q"}, enabled, 0ms};
}
void checkPick(const PieResult& result, const ActionId& action) {
    CHECK(result.outcome == PieResult::Pick);
    REQUIRE(result.action);
    CHECK(*result.action == action);
}
void checkCancelled(const PieResult& result) {
    CHECK(result.outcome == PieResult::Cancelled);
    CHECK_FALSE(result.action);
}
}

TEST_CASE("pie layout fills one through eight items in Blender order") {
    const std::array order{PieSlot::W, PieSlot::E, PieSlot::S, PieSlot::N,
                           PieSlot::NW, PieSlot::NE, PieSlot::SW, PieSlot::SE};
    CHECK(layoutSlots(unpinned(0)).empty());
    for (std::size_t count = 1; count <= order.size(); ++count) {
        CAPTURE(count);
        const auto slots = layoutSlots(unpinned(count));
        REQUIRE(slots.size() == count);
        for (std::size_t i = 0; i < count; ++i) CHECK(slots[i] == order[i]);
    }
    CHECK_THROWS_AS(layoutSlots(unpinned(9)), std::invalid_argument);
}

TEST_CASE("pie layout reserves all pinned slots before filling free slots") {
    auto menu = unpinned(4);
    menu.items[1].slot = PieSlot::W;
    menu.items[3].slot = PieSlot::S;
    CHECK(layoutSlots(menu) == std::vector{PieSlot::E, PieSlot::W, PieSlot::N, PieSlot::S});
    menu.items[2].slot = PieSlot::W;
    CHECK_THROWS_AS(layoutSlots(menu), std::invalid_argument);
    CHECK_THROWS_AS(gesture(menu), std::invalid_argument);
}

TEST_CASE("pie slot directions are unit vectors in y-down logical pixels") {
    CHECK(slotDirection(PieSlot::N) == DVec2{0, -1});
    CHECK(slotDirection(PieSlot::E) == DVec2{1, 0});
    CHECK(slotDirection(PieSlot::S) == DVec2{0, 1});
    CHECK(slotDirection(PieSlot::W) == DVec2{-1, 0});
    for (const auto slot : layoutSlots(unpinned(8)))
        CHECK(glm::length(slotDirection(slot)) == doctest::Approx(1));
    CHECK(slotDirection(PieSlot::NE).x > 0); CHECK(slotDirection(PieSlot::NE).y < 0);
    CHECK(slotDirection(PieSlot::NW).x < 0); CHECK(slotDirection(PieSlot::NW).y < 0);
    CHECK(slotDirection(PieSlot::SE).x > 0); CHECK(slotDirection(PieSlot::SE).y > 0);
    CHECK(slotDirection(PieSlot::SW).x < 0); CHECK(slotDirection(PieSlot::SW).y > 0);
}

TEST_CASE("pie two and four items own 180 and 90 degree sectors") {
    for (int degree = 0; degree < 360; ++degree) {
        // Offset the samples so no point lies on a shared boundary.
        const double angle = glm::radians(degree + 0.25);
        const DVec2 position{100 * std::cos(angle), 100 * std::sin(angle)};
        auto two = gesture(unpinned(2));
        two.input(pointer(position), 10ms);
        CHECK(two.highlighted() == (position.x < 0 ? 0 : 1));
        auto four = gesture(unpinned(4));
        four.input(pointer(position), 10ms);
        const std::size_t expected = std::abs(position.x) > std::abs(position.y)
            ? (position.x < 0 ? 0 : 1) : (position.y > 0 ? 2 : 3);
        CHECK(four.highlighted() == expected);
    }
}

TEST_CASE("pie eight items own 45 degree sectors independent of radius and distance") {
    PieSettings settings; settings.radius = 30;
    const auto slots = layoutSlots(toolPie());
    for (std::size_t i = 0; i < slots.size(); ++i) {
        for (const double angle : {-22.4, 0.0, 22.4}) {
            const auto direction = slotDirection(slots[i]);
            const double c = std::cos(glm::radians(angle)), s = std::sin(glm::radians(angle));
            const DVec2 rotated{c * direction.x - s * direction.y, s * direction.x + c * direction.y};
            for (const double distance : {12.01, 100.0, 100000.0}) {
                auto pie = gesture(toolPie(), settings);
                pie.input(pointer(rotated * distance), 1ms);
                CHECK(pie.highlighted() == i);
            }
        }
    }
}

TEST_CASE("pie dead zone and centre use logical pixel offsets") {
    PieGesture pie{toolPie(), {}, {200, 300}, {"Q"}, enabled, 0ms};
    CHECK_FALSE(pie.highlighted());
    for (const double distance : {0.0, 11.9, 12.0}) {
        pie.input(pointer({200 + distance, 300}), 1ms);
        CHECK_FALSE(pie.highlighted());
    }
    pie.input(pointer({212.01, 300}), 2ms);
    REQUIRE(pie.highlighted());
    CHECK(toolPie().items[*pie.highlighted()].action == "tool.block");
    auto empty = gesture(unpinned(0));
    empty.input(pointer({100, 0}), 1ms);
    CHECK_FALSE(empty.highlighted());
}

TEST_CASE("pie quick tap stays open and left press picks") {
    auto pie = gesture();
    CHECK(pie.input(pointer({6, 0}), 20ms).outcome == PieResult::Pending);
    CHECK(pie.input(key("Q", InputKind::KeyUp), 199ms).outcome == PieResult::Pending);
    CHECK(pie.clickStyle());
    CHECK(pie.input(key("Q", InputKind::KeyUp, true), 200ms).outcome == PieResult::Pending);
    CHECK(pie.input(pointer({100, 0}), 210ms).outcome == PieResult::Pending);
    CHECK(pie.input(pointer({100, 0}, InputKind::Release, PointerButton::Left), 220ms).outcome == PieResult::Pending);
    checkPick(pie.input(pointer({100, 0}, InputKind::Press, PointerButton::Left), 230ms), "tool.block");
}

TEST_CASE("pie click style press in dead zone cancels") {
    auto pie = gesture();
    pie.input(key("Q", InputKind::KeyUp), 50ms);
    checkCancelled(pie.input(pointer({0, 0}, InputKind::Press, PointerButton::Left), 100ms));
}

TEST_CASE("pie hold at timeout and past timeout picks on trigger release") {
    for (const auto release : {200ms, 250ms}) {
        // A small dead zone lets a hold select without crossing the drag threshold.
        PieSettings settings; settings.deadZone = 2;
        auto pie = gesture(toolPie(), settings);
        pie.input(pointer({6, 0}), 10ms);
        CHECK(pie.input(key("Other", InputKind::KeyUp), release).outcome == PieResult::Pending);
        checkPick(pie.input(key("Q", InputKind::KeyUp), release), "tool.block");
        CHECK_FALSE(pie.clickStyle());
    }
}

TEST_CASE("pie ten pixel flick at 50 ms latches drag style") {
    PieSettings settings; settings.deadZone = 2;
    auto pie = gesture(toolPie(), settings);
    pie.input(pointer({10, 0}), 20ms);
    pie.input(pointer({6, 0}), 40ms);
    checkPick(pie.input(key("Q", InputKind::KeyUp), 50ms), "tool.block");
    CHECK_FALSE(pie.clickStyle());
}

TEST_CASE("pie drag threshold is latched even when returning to dead zone") {
    auto pie = gesture();
    pie.input(pointer({7, 0}), 10ms);
    pie.input(pointer({0, 0}), 20ms);
    checkCancelled(pie.input(key("Q", InputKind::KeyUp), 50ms));
    CHECK_FALSE(pie.clickStyle());
    auto hold = gesture();
    checkCancelled(hold.input(key("Q", InputKind::KeyUp), 250ms));
}

TEST_CASE("pie trigger release recognizes chord key after modifiers are released") {
    PieGesture pie{toolPie(), {}, {0, 0}, {"Space", false, true}, enabled, 0ms};
    pie.input(pointer({0, -100}), 10ms);
    CHECK(pie.input(key("Shift", InputKind::KeyUp), 20ms).outcome == PieResult::Pending);
    checkPick(pie.input(key("Space", InputKind::KeyUp, true), 50ms), "tool.vertex");
}

TEST_CASE("pie digits pick all eight directions by numpad layout") {
    const std::array<std::pair<const char*, const char*>, 8> expected{{
        {"8", "tool.vertex"}, {"9", "tool.primitive"}, {"6", "tool.block"}, {"3", "tool.apply"},
        {"2", "tool.entity"}, {"1", "tool.measure"}, {"4", "tool.select"}, {"7", "tool.clip"},
    }};
    for (const bool chord : {false, true}) for (const auto& [digit, action] : expected) {
        auto pie = gesture();
        checkPick(pie.input(key(digit, InputKind::KeyDown, chord), 10ms), action);
    }
    auto pie = gesture(unpinned(2));
    for (const auto* digit : {"5", "0", "8", "9", "3", "2", "1", "7"})
        CHECK(pie.input(key(digit), 10ms).outcome == PieResult::Pending);
    checkPick(pie.input(key("4"), 20ms), "0");
}

TEST_CASE("pie Escape right press Cancel and focus loss cancel") {
    for (int cancellation = 0; cancellation < 5; ++cancellation) {
        auto pie = gesture();
        pie.input(pointer({100, 0}), 10ms);
        PieResult result;
        if (cancellation == 0) result = pie.input(key("Escape"), 20ms);
        if (cancellation == 1) result = pie.input(key("Escape", InputKind::KeyDown, true), 20ms);
        if (cancellation == 2) result = pie.input(pointer({100, 0}, InputKind::Press, PointerButton::Right), 20ms);
        if (cancellation == 3) { InputEvent event; event.kind = InputKind::Cancel; result = pie.input(event, 20ms); }
        if (cancellation == 4) result = pie.focusLost();
        checkCancelled(result);
        CHECK_FALSE(pie.highlighted());
        checkCancelled(pie.input(key("6"), 30ms));
        checkCancelled(pie.tick(100ms));
    }
}

TEST_CASE("pie repeated key events are swallowed without changing gesture") {
    auto pie = gesture();
    pie.input(pointer({100, 0}), 10ms);
    for (const auto* name : {"Q", "6", "Escape"}) {
        auto event = key(name); event.autoRepeat = true;
        CHECK(pie.input(event, 20ms).outcome == PieResult::Pending);
    }
    auto release = key("Q", InputKind::KeyUp); release.autoRepeat = true;
    CHECK(pie.input(release, 30ms).outcome == PieResult::Pending);
    checkPick(pie.input(key("Q", InputKind::KeyUp), 50ms), "tool.block");
    PieGesture digitTrigger{toolPie(), {}, {0, 0}, {"6"}, enabled, 0ms};
    CHECK(digitTrigger.input(key("6"), 10ms).outcome == PieResult::Pending);
}

TEST_CASE("pie disabled items keep their sectors but cannot highlight or pick") {
    bool blockEnabled = false;
    const auto make = [&] {
        return PieGesture{toolPie(), {}, {0, 0}, {"Q"}, [&](const ActionId& action) {
            return action != "tool.block" || blockEnabled;
        }, 0ms};
    };
    auto pie = make();
    pie.input(pointer({100, 0}), 10ms);
    CHECK_FALSE(pie.highlighted());
    CHECK(pie.input(key("6"), 20ms).outcome == PieResult::Pending);
    blockEnabled = true; CHECK(pie.highlighted());
    blockEnabled = false;
    // A disabled item neither picks nor closes the pie; a drag release over one falls back to clicking.
    CHECK(pie.input(key("Q", InputKind::KeyUp), 50ms).outcome == PieResult::Pending);
    CHECK(pie.clickStyle());
    checkPick(pie.input(pointer({-100, 0}, InputKind::Press, PointerButton::Left), 60ms), "tool.select");
    auto click = make();
    click.input(key("Q", InputKind::KeyUp), 50ms);
    CHECK(click.input(pointer({100, 0}, InputKind::Press, PointerButton::Left), 100ms).outcome ==
          PieResult::Pending);
    auto other = make();
    checkPick(other.input(key("4"), 10ms), "tool.select");
}

TEST_CASE("pie confirm on stop waits 20 ms since last actual movement") {
    PieSettings settings; settings.confirmOnStopDistance = 50;
    auto pie = gesture(toolPie(), settings);
    pie.input(pointer({100, 0}), 10ms);
    CHECK(pie.tick(29ms).outcome == PieResult::Pending);
    pie.input(pointer({100, 0}), 29ms);
    checkPick(pie.tick(30ms), "tool.block");
    checkPick(pie.input(key("Q", InputKind::KeyUp), 50ms), "tool.block");
    checkPick(pie.focusLost(), "tool.block");
    auto moving = gesture(toolPie(), settings);
    moving.input(pointer({100, 0}), 10ms);
    moving.input(pointer({101, 0}), 29ms);
    CHECK(moving.tick(48ms).outcome == PieResult::Pending);
    checkPick(moving.tick(49ms), "tool.block");
}

TEST_CASE("pie confirm on stop respects distance disabled state and off setting") {
    PieSettings settings; settings.confirmOnStopDistance = 50;
    auto pie = gesture(toolPie(), settings);
    for (const double distance : {0.0, 12.0, 49.0, 50.0}) {
        pie.input(pointer({distance, 0}), 10ms);
        CHECK(pie.tick(100ms).outcome == PieResult::Pending);
    }
    auto off = gesture();
    off.input(pointer({1000, 0}), 10ms);
    CHECK(off.tick(10000ms).outcome == PieResult::Pending);
    PieGesture disabled{toolPie(), settings, {0, 0}, {"Q"}, [](const ActionId&) { return false; }, 0ms};
    disabled.input(pointer({100, 0}), 10ms);
    CHECK(disabled.tick(30ms).outcome == PieResult::Pending);
    settings.confirmStill = 40ms;
    auto slower = gesture(toolPie(), settings);
    slower.input(pointer({100, 0}), 10ms);
    CHECK(slower.tick(49ms).outcome == PieResult::Pending);
    checkPick(slower.tick(50ms), "tool.block");
}

TEST_CASE("pie open animation clamps injected time and supports reduced motion") {
    PieGesture pie{toolPie(), {}, {0, 0}, {"Q"}, enabled, 100ms};
    CHECK(pie.openProgress(0ms) == 0);
    CHECK(pie.openProgress(100ms) == 0);
    CHECK(pie.openProgress(130ms) == doctest::Approx(0.5));
    CHECK(pie.openProgress(160ms) == 1);
    CHECK(pie.openProgress(1000ms) == 1);
    PieSettings settings; settings.animation = 0ms;
    CHECK(gesture(toolPie(), settings).openProgress(0ms) == 1);
}

TEST_CASE("tool pie pins every registry tool action to the fixed layout") {
    const std::array<std::pair<PieSlot, const char*>, 8> expected{{
        {PieSlot::NW, "tool.clip"}, {PieSlot::N, "tool.vertex"}, {PieSlot::NE, "tool.primitive"},
        {PieSlot::W, "tool.select"}, {PieSlot::E, "tool.block"}, {PieSlot::SW, "tool.measure"},
        {PieSlot::S, "tool.entity"}, {PieSlot::SE, "tool.apply"},
    }};
    const auto& menu = toolPie();
    REQUIRE(menu.items.size() == 8);
    CHECK_FALSE(menu.title.empty());
    std::set<ActionId> actions;
    std::set<PieSlot> slots;
    for (const auto& item : menu.items) {
        REQUIRE(item.slot);
        CHECK(slots.insert(*item.slot).second);
        CHECK(actions.insert(item.action).second);
        CHECK_FALSE(item.label.empty());
        REQUIRE(actionRegistry().find(item.action));
        CHECK(actionRegistry().find(item.action)->category == "Tool");
        for (const auto& [slot, action] : expected) if (*item.slot == slot) CHECK(item.action == action);
    }
    CHECK(layoutSlots(menu).size() == 8);
    for (const auto& action : actionRegistry().entries())
        if (action.category == "Tool" && action.id.starts_with("tool.")) CHECK(actions.contains(action.id));
}
