// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/ui/animation.hpp"

#include <algorithm>

namespace filepan::ui {

void FrameClock::start(Ticker *ticker) {
    if (std::ranges::find(tickers_, ticker) != tickers_.end()) {
        return;
    }
    const bool wasIdle = !active();
    tickers_.push_back(ticker);
    if (wasIdle && !ticking_ && requestFrame) {
        requestFrame();
    }
}

void FrameClock::stop(Ticker *ticker) {
    auto it = std::ranges::find(tickers_, ticker);
    if (it == tickers_.end()) {
        return;
    }
    if (ticking_) {
        *it = nullptr;  // Swept after the tick.
    } else {
        tickers_.erase(it);
    }
}

void FrameClock::tick(std::int64_t nowNs) {
    now_ = nowNs;
    ticking_ = true;
    // Tickers started during the tick are appended and tick this frame too.
    for (std::size_t i = 0; i < tickers_.size(); ++i) {
        Ticker *ticker = tickers_[i];
        if (ticker != nullptr && !ticker->tick(nowNs)) {
            tickers_[i] = nullptr;
        }
    }
    ticking_ = false;
    std::erase(tickers_, nullptr);
}

FrameClock &frameClock() {
    thread_local FrameClock clock;
    return clock;
}

float CubicBezier::operator()(float t) const {
    if (t <= 0) {
        return 0;
    }
    if (t >= 1) {
        return 1;
    }
    const auto curve = [](double p1, double p2, double s) {
        const double u = 1 - s;
        return 3 * u * u * s * p1 + 3 * u * s * s * p2 + s * s * s;
    };
    const auto slope = [](double p1, double p2, double s) {
        const double u = 1 - s;
        return 3 * u * u * p1 + 6 * u * s * (p2 - p1) + 3 * s * s * (1 - p2);
    };
    // Solve x(s) = t: Newton's method, falling back to bisection where the slope is flat.
    double s = t;
    for (int i = 0; i < 8; ++i) {
        const double error = curve(x1_, x2_, s) - t;
        if (std::abs(error) < 1e-6) {
            return static_cast<float>(curve(y1_, y2_, s));
        }
        const double d = slope(x1_, x2_, s);
        if (std::abs(d) < 1e-6) {
            break;
        }
        s -= error / d;
    }
    double low = 0;
    double high = 1;
    s = t;
    for (int i = 0; i < 40; ++i) {
        const double x = curve(x1_, x2_, s);
        if (std::abs(x - t) < 1e-7) {
            break;
        }
        if (x < t) {
            low = s;
        } else {
            high = s;
        }
        s = (low + high) / 2;
    }
    return static_cast<float>(curve(y1_, y2_, s));
}

SpringSimulation::SpringSimulation(float stiffness, float dampingRatio, float position, float velocity)
    : omega_(std::sqrt(std::max(stiffness, 1e-3f))), zeta_(std::max(dampingRatio, 0.0f)), x0_(position),
      v0_(velocity) {}

float SpringSimulation::position(float seconds) const {
    const double t = seconds;
    const double w = omega_;
    const double z = zeta_;
    const double x0 = x0_;
    const double v0 = v0_;
    if (std::abs(z - 1) < 1e-4) {
        return static_cast<float>((x0 + (v0 + w * x0) * t) * std::exp(-w * t));
    }
    if (z < 1) {
        const double wd = w * std::sqrt(1 - z * z);
        const double b = (v0 + z * w * x0) / wd;
        return static_cast<float>(std::exp(-z * w * t) * (x0 * std::cos(wd * t) + b * std::sin(wd * t)));
    }
    const double root = std::sqrt(z * z - 1);
    const double r1 = -w * (z - root);
    const double r2 = -w * (z + root);
    const double c1 = (v0 - r2 * x0) / (r1 - r2);
    const double c2 = x0 - c1;
    return static_cast<float>(c1 * std::exp(r1 * t) + c2 * std::exp(r2 * t));
}

float SpringSimulation::velocity(float seconds) const {
    const double t = seconds;
    const double w = omega_;
    const double z = zeta_;
    const double x0 = x0_;
    const double v0 = v0_;
    if (std::abs(z - 1) < 1e-4) {
        const double c = v0 + w * x0;
        return static_cast<float>(std::exp(-w * t) * (c - w * (x0 + c * t)));
    }
    if (z < 1) {
        const double a = z * w;
        const double wd = w * std::sqrt(1 - z * z);
        const double b = (v0 + a * x0) / wd;
        return static_cast<float>(std::exp(-a * t) *
                                  ((-a * x0 + b * wd) * std::cos(wd * t) + (-a * b - x0 * wd) * std::sin(wd * t)));
    }
    const double root = std::sqrt(z * z - 1);
    const double r1 = -w * (z - root);
    const double r2 = -w * (z + root);
    const double c1 = (v0 - r2 * x0) / (r1 - r2);
    const double c2 = x0 - c1;
    return static_cast<float>(c1 * r1 * std::exp(r1 * t) + c2 * r2 * std::exp(r2 * t));
}

}  // namespace filepan::ui
