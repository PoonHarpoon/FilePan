// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <memory>
#include <string>

#include "filepan/ui/animation.hpp"
#include "filepan/ui/components/common.hpp"
#include "filepan/ui/node.hpp"

// Hero objects (§7.8): only on screens with room for one, the storage home and empty states.
namespace filepan::ui::components {

struct OrbProps {
    float size = 120;  // dp; the orbit rings reach a little past it.
    // Slowly turns the orbit rings. Off by default (it keeps frames coming), and never under
    // reduced motion.
    bool animated = false;
};

// A sphere shaded from the accent to the secondary color, two tilted orbit rings passing behind
// and in front of it, and a small panel tile with a crosshair. Decorative: hidden from semantics.
class Orb final : public Node, private FrameClock::Ticker {
public:
    static std::shared_ptr<Orb> create(OrbProps props = {});
    ~Orb() override;

    [[nodiscard]] bool animating() const { return ticking_; }

protected:
    void paint(PaintContext &context) override;

private:
    bool tick(std::int64_t nowNs) override;
    void updateTicking(bool on);

    OrbProps props_;
    float phase_ = 0;
    std::int64_t startNs_ = -1;
    bool ticking_ = false;
};

struct EmptyStateProps {
    std::string title = "Empty";  // Display type, caps.
    std::string path;             // Mono, beneath.
    std::string message;          // Optional body line.
    float orbSize = 128;
};

// An orb, `EMPTY` in display type and the path in mono.
NodePtr EmptyState(EmptyStateProps props);

}  // namespace filepan::ui::components
