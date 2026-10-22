#include "editor/selection.hpp"

#include "editor/selection_ops.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>

namespace apex::editor {
namespace {
constexpr std::uint64_t kMagic = 0x4150455853454c;
}

Selection::Selection(const Selection& other)
    : mode_(other.mode_), ignoreGroups_(other.ignoreGroups_), ordered_(other.ordered_),
      active_(other.active_), members_(other.members_) {}

Selection& Selection::operator=(const Selection& other) {
    if (mode_ == other.mode_ && ignoreGroups_ == other.ignoreGroups_ &&
        ordered_ == other.ordered_ && active_ == other.active_) return *this;
    mode_ = other.mode_;
    ignoreGroups_ = other.ignoreGroups_;
    ordered_ = other.ordered_;
    active_ = other.active_;
    members_ = other.members_;
    notify();
    return *this;
}

void Selection::notify() {
    ++changed_;
    // A callback may unregister itself or another callback. New observers wait until
    // the next change. Like Document, observers must not mutate the observed state.
    const auto snapshot = observers_;
    for (const auto& [token, callback] : snapshot) {
        if (observers_.contains(token)) callback(*this);
    }
}

std::size_t Selection::observe(Observer observer) {
    const auto token = nextObserver_++;
    observers_.emplace(token, std::move(observer));
    return token;
}
void Selection::unobserve(std::size_t token) { observers_.erase(token); }

void Selection::setMode(Mode mode) {
    if (mode_ == mode) return;
    if (mode < Mode::Groups || mode > Mode::Faces) throw std::invalid_argument("invalid selection mode");
    mode_ = mode;
    ordered_.clear();
    members_.clear();
    active_.reset();
    notify();
}
void Selection::setIgnoreGroups(bool ignore) {
    if (ignoreGroups_ == ignore) return;
    ignoreGroups_ = ignore;
    notify();
}
void Selection::validate(Target target) const {
    if (mode_ == Mode::Faces ? (!std::holds_alternative<SideId>(target) || std::get<SideId>(target) < 0)
                             : (!std::holds_alternative<NodeId>(target) || std::get<NodeId>(target) == 0)) {
        throw std::invalid_argument("selection target differs from mode or has invalid ID");
    }
}

bool Selection::contains(Target target) const {
    return members_.contains(target);
}
void Selection::select(Target target) { replace(std::span(&target, 1)); }
void Selection::add(Target target) {
    validate(target);
    const bool present = contains(target);
    if (present && active_ == target) return;
    if (!present) {
        ordered_.push_back(target);
        members_.insert(target);
    }
    active_ = target;
    notify();
}
void Selection::toggle(Target target) {
    validate(target);
    if (contains(target)) remove(target);
    else add(target);
}
void Selection::remove(Target target) {
    const auto it = std::ranges::find(ordered_, target);
    if (it == ordered_.end()) return;
    ordered_.erase(it);
    members_.erase(target);
    if (active_ == target) active_ = ordered_.empty() ? std::nullopt : std::optional(ordered_.back());
    notify();
}
void Selection::clear() { replace({}); }
void Selection::replace(std::span<const Target> targets) {
    std::vector<Target> next;
    std::set<Target> seen;
    for (const auto target : targets) {
        validate(target);
        if (seen.insert(target).second) next.push_back(target);
    }
    const auto active = next.empty() ? std::nullopt : std::optional(next.back());
    if (next == ordered_ && active == active_) return;
    ordered_ = std::move(next);
    members_ = std::move(seen);
    active_ = active;
    notify();
}

void Selection::selectAll(const document::Document& doc, const PickIndex& picks) {
    GroupIndex groups(doc);
    editor::selectAll(*this, picks, groups);
}
void Selection::invert(const document::Document& doc, const PickIndex& picks) {
    GroupIndex groups(doc);
    editor::invert(*this, picks, groups);
}
void Selection::prune(const document::Document& doc) {
    auto next = ordered_;
    std::erase_if(next, [&](Target target) {
        if (const auto* side = std::get_if<SideId>(&target)) return !doc.side(*side);
        return !doc.handle(std::get<NodeId>(target));
    });
    if (next == ordered_) return;
    ordered_ = std::move(next);
    members_ = {ordered_.begin(), ordered_.end()};
    if (active_ && !contains(*active_)) active_ = ordered_.empty() ? std::nullopt : std::optional(ordered_.back());
    notify();
}

document::SelectionBlob Selection::toBlob() const {
    document::SelectionBlob result{kMagic, 1, static_cast<std::uint64_t>(mode_),
                                    ignoreGroups_ ? 1ULL : 0ULL, 0, ordered_.size()};
    for (std::size_t i = 0; i < ordered_.size(); ++i) {
        if (active_ == ordered_[i]) result[4] = i + 1;
        result.push_back(std::visit([](auto id) { return static_cast<std::uint64_t>(id); }, ordered_[i]));
    }
    return result;
}
bool Selection::fromBlob(const document::SelectionBlob& blob) {
    if (blob.size() < 6 || blob[0] != kMagic || blob[1] != 1 ||
        blob[2] > static_cast<std::uint64_t>(Mode::Faces) || blob[3] > 1 ||
        blob[5] != blob.size() - 6 || blob[4] > blob[5] || (blob[5] && !blob[4])) return false;
    Selection next(static_cast<Mode>(blob[2]));
    next.ignoreGroups_ = blob[3] != 0;
    std::set<std::uint64_t> seen;
    for (std::size_t i = 6; i < blob.size(); ++i) {
        const auto id = blob[i];
        if (!seen.insert(id).second) return false;
        if (next.mode_ == Mode::Faces) {
            if (id > static_cast<std::uint64_t>(std::numeric_limits<SideId>::max())) return false;
            next.ordered_.emplace_back(static_cast<SideId>(id));
        } else {
            if (!id) return false;
            next.ordered_.emplace_back(NodeId{id});
        }
    }
    next.members_ = {next.ordered_.begin(), next.ordered_.end()};
    if (blob[4]) next.active_ = next.ordered_[blob[4] - 1];
    *this = next;
    return true;
}

} // namespace apex::editor
