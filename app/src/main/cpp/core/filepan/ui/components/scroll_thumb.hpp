// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <functional>
#include <memory>
#include <optional>

#include "filepan/ui/node.hpp"
#include "filepan/ui/scroll.hpp"

// A thin scrollbar for a long list: a narrow strip over the list's end edge with a thumb that
// shows where the list is. Pressing it widens the thumb; dragging it moves the list, so a long
// folder is quick to cross. It draws nothing (and drags nothing) while the content is short.
namespace filepan::ui::components {

class ScrollThumb final : public Node {
public:
    // The strip's width, in dp: within a row's padding, so it doesn't cover what rows show.
    static constexpr float kWidth = 12;

    // scroll must outlive the thumb. The owner places it: absolute, at the end edge, its top and
    // bottom where the list's content starts and ends. dependencies is tracked, so the thumb
    // redraws when the content changes length: read what changes it (a model's version) in it.
    static std::shared_ptr<ScrollThumb> create(ScrollNode &scroll, std::function<void()> dependencies = {});
    explicit ScrollThumb(ScrollNode &scroll) : scroll_(scroll) {}

    [[nodiscard]] bool expanded() const { return expanded_; }
    // Where the thumb is along the strip, in dp; none while the content is short.
    struct Geometry {
        float top = 0;
        float length = 0;
    };
    [[nodiscard]] std::optional<Geometry> geometry() const;

protected:
    void paint(PaintContext &context) override;

private:
    void setExpanded(bool expanded);
    // Moves the thumb so its middle is at y (dp from the strip's top).
    void centerAt(float y);
    void dragBy(float dp);

    ScrollNode &scroll_;
    bool expanded_ = false;
};

}  // namespace filepan::ui::components
