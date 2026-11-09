// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/ui/components/hero.hpp"

#include <cmath>
#include <numbers>

#include "filepan/ui/components/recipes.hpp"

namespace filepan::ui::components {

namespace {

constexpr float kPi = std::numbers::pi_v<float>;

gfx::RoundedRect circle(gfx::Vec2 center, float radius) {
    return {{center.x - radius, center.y - radius, radius * 2, radius * 2}, gfx::CornerRadii::uniform(radius)};
}

gfx::Transform rotation(float radians) {
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    return {c, s, -s, c, 0, 0};
}

struct Ring {
    float radius;  // Fraction of the sphere's radius.
    float tilt;    // Radians.
    float squash;  // The ellipse's minor/major ratio.
    float speed;   // Satellite turns per animation unit.
    bool accent;
};

constexpr Ring kRings[] = {
        {1.52f, -0.30f, 0.26f, 1.0f, false},
        {1.30f, 0.42f, 0.22f, -1.4f, true},
};

}  // namespace

std::shared_ptr<Orb> Orb::create(OrbProps props) {
    auto node = std::make_shared<Orb>();
    Orb *raw = node.get();
    node->setDebugName("Orb");
    node->props_ = props;
    node->setStyle({.width = props.size, .height = props.size});
    node->setHitTestBehavior(HitTestBehavior::none);
    node->updateSemantics([](Semantics &s) { s.hidden = true; });
    if (props.animated) {
        node->bind([raw] {
            const bool reduced = environment().reducedMotion();
            untrack([&] { raw->updateTicking(!reduced); });
        });
    }
    return node;
}

Orb::~Orb() { updateTicking(false); }

void Orb::updateTicking(bool on) {
    if (on == ticking_) {
        return;
    }
    ticking_ = on;
    if (on) {
        startNs_ = -1;
        frameClock().start(this);
    } else {
        frameClock().stop(this);
    }
}

bool Orb::tick(std::int64_t nowNs) {
    if (!ticking_) {
        return false;
    }
    if (startNs_ < 0) {
        startNs_ = nowNs - static_cast<std::int64_t>(phase_ / 0.35f * 1e9f);
    }
    phase_ = static_cast<float>(nowNs - startNs_) * 1e-9f * 0.35f;
    markPaintDirty();
    return true;
}

void Orb::paint(PaintContext &context) {
    const Theme &theme = currentTheme();
    const ColorTokens &c = theme.colors;
    const gfx::Rect bounds = context.bounds();
    const gfx::Vec2 center = bounds.center();
    const float r = std::min(bounds.width, bounds.height) / 2 * 0.62f;
    const float stroke = std::max(1.0f, context.px(1.25f));

    const auto ringTransform = [&](const Ring &ring) {
        return gfx::Transform::translation(center) * rotation(ring.tilt) * gfx::Transform::scale(1, ring.squash);
    };
    const auto ringColor = [&](const Ring &ring, float alpha) {
        return ring.accent ? legible(c.accent, c.ground, 3).withAlpha(alpha) : c.ink.withAlpha(alpha * 0.8f);
    };
    // A satellite on the ring at its current angle; front reports whether it's in front.
    const auto satellite = [&](const Ring &ring, bool front) {
        const float angle = phase_ * ring.speed * 2 * kPi + (ring.accent ? 2.2f : 0.6f);
        const float radius = r * ring.radius;
        const gfx::Vec2 local{radius * std::cos(angle), radius * std::sin(angle)};
        if ((local.y > 0) != front) {
            return;
        }
        const gfx::Vec2 p = ringTransform(ring).apply(local);
        const float size = context.px(ring.accent ? 3.5f : 2.5f);
        const gfx::Color color = ring.accent ? c.accent : c.ink;
        context.list.fill(circle(p, size), color);
    };
    const auto drawRing = [&](const Ring &ring, bool front) {
        const float radius = r * ring.radius;
        context.list.pushTransform(ringTransform(ring));
        if (front) {
            context.list.pushClip({{-radius * 1.2f, 0, radius * 2.4f, radius * 1.2f}, {}});
        }
        // The stroke is squashed with the ellipse, so it thins at the far and near sides like a
        // ring seen at an angle.
        context.list.stroke(circle({0, 0}, radius), stroke / ring.squash * 0.55f, ringColor(ring, front ? 0.75f : 0.4f));
        if (front) {
            context.list.pop();
        }
        context.list.pop();
    };

    for (const Ring &ring : kRings) {
        drawRing(ring, false);
        satellite(ring, false);
    }

    // The sphere.
    const gfx::RoundedRect sphere = circle(center, r);
    // The accent fading straight down to the secondary: no highlight, rim light or soft shadow.
    context.list.fill(sphere, recipes::verticalGradient(sphere.rect, {{0, c.accent}, {1, darken(c.secondary, 0.3f)}}));

    for (const Ring &ring : kRings) {
        drawRing(ring, true);
        satellite(ring, true);
    }

    // A small panel tile with a crosshair, resting on the sphere's lower right.
    const float tile = r * 0.62f;
    const gfx::Rect tileRect{center.x + r * 0.42f, center.y + r * 0.36f, tile, tile};
    const gfx::RoundedRect tileShape{tileRect, gfx::CornerRadii::uniform(theme.cornerPx(tile * 0.28f, context.density))};
    recipes::glass(context.list, tileShape, theme, context.density);
    recipes::crosshair(context.list, tileRect.center(), tile * 0.22f, legible(c.accent, c.ground, 3), context.density);
}

NodePtr EmptyState(EmptyStateProps props) {
    const Theme &initial = currentTheme();
    auto node = Box::create({.justify = Justify::center, .alignItems = Align::center, .gap = 10,
                             .padding = Edges::all(24)});
    node->setDebugName("EmptyState");
    auto orb = Orb::create({.size = props.orbSize});
    orb->setStyle({.margin = {0, 0, 0, 12}, .width = props.orbSize, .height = props.orbSize});
    node->appendChild(orb);
    // The utilitarian style says it plainly, at title size.
    const gfx::TextStyle display = initial.style.utilitarian ? initial.type.title : withSize(initial.type.display, 48);
    auto title = Text::create(props.title, display);
    node->appendChild(title);
    std::shared_ptr<Text> message;
    if (!props.message.empty()) {
        message = Text::create(props.message, initial.type.bodySmall);
        message->setAlign(gfx::TextAlign::center);
        node->appendChild(message);
    }
    auto path = Text::create(props.path, initial.type.label);
    path->setMaxLines(1);
    path->setOverflow(gfx::TextOverflow::ellipsisMiddle);
    path->setStyle({.shrink = 1});
    node->appendChild(path);
    node->updateSemantics([&](Semantics &s) { s.mergeDescendants = true; });
    node->bind([o = orb.get(), t = title.get(), m = message.get(), p = path.get()] {
        const Theme &th = theme();
        untrack([&] {
            o->updateStyle([&](Style &s) { s.display = th.style.marks; });
            t->setColor(ink(th, 1));
            if (m != nullptr) {
                m->setColor(ink(th, th.text.body));
            }
            p->setColor(textInk(th, th.text.label, 11));
        });
    });
    return node;
}

}  // namespace filepan::ui::components
