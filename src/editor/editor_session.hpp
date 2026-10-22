#pragma once

#include "editor/actions.hpp"
#include "editor/selection_ops.hpp"

#include <memory>

namespace apex::editor {

// One document's editor state. The attached document must outlive the session or
// detach(). Selection-only gestures never write to Document or its undo history.
class EditorSession {
public:
    EditorSession();
    ~EditorSession();
    EditorSession(const EditorSession&) = delete;
    EditorSession& operator=(const EditorSession&) = delete;

    void attach(document::Document& document);
    void detach();
    document::Document* document() const { return document_; }
    Selection& selection() { return selection_; }
    const Selection& selection() const { return selection_; }
    const PickIndex& picks() const { return *picks_; }
    const GroupIndex& groups() const { return *groups_; }
    const CycleState& cycle() const { return cycle_; }
    const ViewState& viewState() const { return view_; }
    void setViewState(ViewState view);
    std::optional<Target> hoverTarget() const { return hover_; }
    std::optional<NodeId> hoverDrawable() const { return hoverDrawable_; }
    void clearHover();
    void pointerHover(Ray ray);
    void pointerClick(Ray ray, Modifiers modifiers = {}, bool sameSpot = false);
    void marquee(std::span<const Plane> planes, Modifiers modifiers = {});
    void selectNode(NodeId node, Modifiers modifiers = {});
    MarqueeMode marqueeMode() const { return marqueeMode_; }
    void setMarqueeMode(MarqueeMode mode);

    const Preset& preset() const { return preset_; }
    const Keymap& keymap() const { return keymap_; }
    void setPreset(const Preset& preset);
    bool perform(const ActionId& action);
    // Document edits go through this seam to capture before/after selection metadata.
    // Direct Document edits still update indices and prune dead selection targets.
    void edit(std::string label, const std::function<void(document::Transaction&)>& operation);

    using Observer = std::function<void()>;
    std::size_t observe(Observer observer);
    void unobserve(std::size_t token);

private:
    document::Document* document_ = nullptr;
    std::size_t documentObserver_ = 0;
    std::unique_ptr<PickIndex> picks_;
    std::unique_ptr<GroupIndex> groups_;
    Selection selection_;
    CycleState cycle_;
    ViewState view_;
    std::optional<Target> hover_;
    std::optional<NodeId> hoverDrawable_;
    MarqueeMode marqueeMode_ = MarqueeMode::Enclosed;
    Preset preset_;
    Keymap keymap_;
    std::map<std::size_t, Observer> observers_;
    std::size_t nextObserver_ = 1;
    void notify();
};

} // namespace apex::editor
