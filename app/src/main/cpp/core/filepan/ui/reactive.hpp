// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

// Fine-grained reactivity in the style of SolidJS.
//
// - Signal<T> holds a value. Reading it inside a Memo or an Effect subscribes that computation;
//   setting it marks subscribers stale.
// - Memo<T> derives a value from signals and other memos. It's lazy and cached: it recomputes
//   only when read after one of its sources changed, and stops propagation if the new value
//   compares equal to the old one.
// - Effect runs a side effect (usually writing a node property) once on creation and again
//   whenever something it read changes. Effects run in height order (an effect that reads a memo
//   runs after effects that read only signals) and pull their memos up to date before running,
//   so they never see a half-updated graph (no glitches).
//
// Ownership: every Memo and Effect belongs to the Owner that was current when it was created: a
// Scope, a node's scope, or the Effect that created it. Disposing an owner disposes everything it
// owns and runs its onCleanup callbacks. An Effect disposes what it created on each re-run, so
// building nodes or effects inside an effect doesn't leak.
//
// Scheduling: with no scheduler installed (host tests), a write outside batch() flushes effects
// synchronously, like Solid. UiRoot installs a scheduler, after which writes only queue effects
// and request a frame; the frame flushes them before layout. Either way batch() coalesces.
//
// Everything here is single-threaded: only the UI thread may touch signals. Other threads post
// closures to UiLoop.
namespace filepan::ui {

namespace detail {

class Computation;
class Owner;

enum class ComputationState : std::uint8_t { clean, check, dirty };

// Something a computation can depend on: a Signal or a Memo.
class Source {
public:
    Source() = default;
    Source(const Source &) = delete;
    Source &operator=(const Source &) = delete;
    virtual ~Source();

    // Subscribes the running computation, if any.
    void track();
    // Marks observers dirty after a write and flushes or schedules effects.
    void notifyWrite();
    // Brings a memo up to date; nothing for a signal.
    virtual void refresh() {}
    [[nodiscard]] virtual int height() const { return 0; }

    void removeObserver(Computation *observer);

    std::vector<Computation *> observers_;
};

// Owns memos and effects created while it's current, plus cleanup callbacks.
class Owner : public std::enable_shared_from_this<Owner> {
public:
    Owner() = default;
    Owner(const Owner &) = delete;
    Owner &operator=(const Owner &) = delete;
    virtual ~Owner();

    virtual void dispose();
    void addCleanup(std::function<void()> cleanup);
    void adopt(std::shared_ptr<Owner> child);
    [[nodiscard]] bool disposed() const { return disposed_; }

protected:
    // Disposes owned children and runs cleanups, but leaves this owner usable.
    void reset();

    std::vector<std::shared_ptr<Owner>> owned_;
    std::vector<std::function<void()>> cleanups_;
    bool disposed_ = false;
};

class Computation : public Owner {
public:
    ~Computation() override;

    void dispose() override;

    void markStale(ComputationState state);
    void updateIfNecessary();
    void addSource(Source *source);
    [[nodiscard]] int computationHeight() const { return height_; }

protected:
    // Runs fn with this computation as the listener and owner, then updates subscriptions.
    template <typename F>
    void runTracked(F &&fn);

    virtual void execute() = 0;
    [[nodiscard]] virtual bool isEffect() const { return false; }
    // The memo's observers, or null for an effect.
    virtual Source *asSource() { return nullptr; }

private:
    void beginTracking();
    void endTracking();

    friend class Source;
    friend class Runtime;

    std::vector<Source *> sources_;
    std::vector<Source *> newSources_;
    std::size_t trackIndex_ = 0;
    Computation *previousListener_ = nullptr;
    Owner *previousOwner_ = nullptr;
    ComputationState state_ = ComputationState::dirty;
    int height_ = 1;
};

// Per-thread reactive state.
class Runtime {
public:
    Computation *listener = nullptr;
    Owner *owner = nullptr;
    int batchDepth = 0;
    bool flushing = false;
    bool scheduled = false;
    // If set, called once when effects get queued instead of flushing synchronously.
    std::function<void()> scheduler;

    void enqueue(Computation *effect);
    // Runs queued effects until none are left.
    void flush();
    void afterWrite();
    [[nodiscard]] bool hasPendingEffects() const { return !queue_.empty(); }

private:
    std::vector<std::shared_ptr<Owner>> queue_;
};

Runtime &runtime();

template <typename F>
void Computation::runTracked(F &&fn) {
    reset();
    beginTracking();
    std::forward<F>(fn)();
    endTracking();
}

template <typename T>
class SignalState final : public Source {
public:
    template <typename... Args>
    explicit SignalState(Args &&...args) : value(std::forward<Args>(args)...) {}

    T value;
};

template <typename T>
class MemoState final : public Computation, public Source {
public:
    explicit MemoState(std::function<T()> fn) : fn_(std::move(fn)) {}

    const T &read() {
        Source::track();
        Computation::updateIfNecessary();
        return *value_;
    }
    const T &peek() {
        Computation::updateIfNecessary();
        return *value_;
    }

    void refresh() override { Computation::updateIfNecessary(); }
    [[nodiscard]] int height() const override { return computationHeight(); }

protected:
    void execute() override {
        std::optional<T> next;
        runTracked([&] { next.emplace(fn_()); });
        bool changed = !value_.has_value();
        if constexpr (std::equality_comparable<T>) {
            changed = changed || !(*value_ == *next);
        } else {
            changed = true;
        }
        value_ = std::move(next);
        if (changed) {
            // Observers are at least "check"; they now know a source really changed.
            for (Computation *observer : observers_) {
                observer->markStale(ComputationState::dirty);
            }
        }
    }
    Source *asSource() override { return this; }

private:
    std::function<T()> fn_;
    std::optional<T> value_;
};

class EffectState final : public Computation {
public:
    explicit EffectState(std::function<void()> fn) : fn_(std::move(fn)) {}

    void run() { updateIfNecessary(); }

protected:
    void execute() override { runTracked(fn_); }
    [[nodiscard]] bool isEffect() const override { return true; }

private:
    std::function<void()> fn_;
};

}  // namespace detail

// A reactive value. Copies share the same state, so a Signal can be captured by value.
template <typename T>
class Signal {
public:
    Signal()
        requires std::default_initializable<T>
        : state_(std::make_shared<detail::SignalState<T>>()) {}
    explicit Signal(T value) : state_(std::make_shared<detail::SignalState<T>>(std::move(value))) {}

    // Tracked read: subscribes the running memo or effect.
    const T &get() const {
        state_->track();
        return state_->value;
    }
    const T &operator()() const { return get(); }
    // Untracked read.
    [[nodiscard]] const T &peek() const { return state_->value; }

    // Notifies subscribers unless the value compares equal.
    void set(T value) const {
        if constexpr (std::equality_comparable<T>) {
            if (state_->value == value) {
                return;
            }
        }
        state_->value = std::move(value);
        state_->notifyWrite();
    }
    // Mutates in place and always notifies.
    template <typename F>
    void update(F &&fn) const {
        std::forward<F>(fn)(state_->value);
        state_->notifyWrite();
    }

    bool operator==(const Signal &other) const { return state_ == other.state_; }

private:
    std::shared_ptr<detail::SignalState<T>> state_;
};

// A derived value, recomputed lazily when read after a source changed.
template <typename T>
class Memo {
public:
    template <typename F>
        requires std::is_invocable_r_v<T, F>
    explicit Memo(F &&fn)
        : state_(std::make_shared<detail::MemoState<T>>(std::function<T()>(std::forward<F>(fn)))) {
        if (auto *owner = detail::runtime().owner) {
            owner->adopt(state_);
        }
    }

    const T &get() const { return state_->read(); }
    const T &operator()() const { return get(); }
    [[nodiscard]] const T &peek() const { return state_->peek(); }

private:
    std::shared_ptr<detail::MemoState<T>> state_;
};

// A handle to an effect. The effect lives while its owner or a handle keeps it; with no owner
// (outside any Scope), keep the handle or the effect is disposed right away.
class Effect {
public:
    Effect() = default;
    explicit Effect(std::shared_ptr<detail::EffectState> state) : state_(std::move(state)) {}

    void dispose() {
        if (state_) {
            state_->dispose();
            state_.reset();
        }
    }
    [[nodiscard]] bool active() const { return state_ && !state_->disposed(); }

private:
    std::shared_ptr<detail::EffectState> state_;
};

// Creates an effect owned by the current owner and runs it once right away.
Effect effect(std::function<void()> fn);

// A root owner, like Solid's createRoot. Disposes everything created under it when destroyed.
class Scope {
public:
    Scope();
    ~Scope();
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;

    // Runs fn with this scope as the owner and no listener (reads inside aren't tracked).
    template <typename F>
    decltype(auto) run(F &&fn) {
        auto &rt = detail::runtime();
        struct Restore {
            detail::Runtime &rt;
            detail::Owner *owner;
            detail::Computation *listener;
            ~Restore() {
                rt.owner = owner;
                rt.listener = listener;
            }
        } restore{rt, rt.owner, rt.listener};
        rt.owner = owner_.get();
        rt.listener = nullptr;
        return std::forward<F>(fn)();
    }

    // Disposes what's owned so far; the scope stays usable.
    void dispose();

private:
    class RootOwner;
    std::shared_ptr<detail::Owner> owner_;
};

// Registers a callback on the current owner: it runs before the owning effect re-runs, or when
// the owner is disposed.
void onCleanup(std::function<void()> cleanup);

// Runs fn with writes coalesced: effects run once at the end (or at the next frame).
template <typename F>
decltype(auto) batch(F &&fn) {
    auto &rt = detail::runtime();
    struct End {
        detail::Runtime &rt;
        ~End() {
            if (--rt.batchDepth == 0) {
                rt.afterWrite();
            }
        }
    };
    ++rt.batchDepth;
    End end{rt};
    return std::forward<F>(fn)();
}

// Runs fn without subscribing the running computation to what it reads.
template <typename F>
decltype(auto) untrack(F &&fn) {
    auto &rt = detail::runtime();
    struct Restore {
        detail::Runtime &rt;
        detail::Computation *listener;
        ~Restore() { rt.listener = listener; }
    } restore{rt, rt.listener};
    rt.listener = nullptr;
    return std::forward<F>(fn)();
}

// Runs queued effects now. UiRoot calls this before layout; tests may call it after setting a
// scheduler.
void flushEffects();

// A property that's either constant or derived from signals: components take these so callers
// can pass a literal or a lambda reading signals.
template <typename T>
class Prop {
public:
    Prop() = default;
    Prop(T value) : value_(std::move(value)) {}  // NOLINT(google-explicit-constructor)
    // Anything that converts to T, so a Prop<std::string> takes a string literal.
    template <typename U>
        requires std::is_convertible_v<U, T> && (!std::is_same_v<std::remove_cvref_t<U>, T>) &&
                 (!std::is_invocable_v<U>)
    Prop(U &&value) : value_(std::forward<U>(value)) {}  // NOLINT(google-explicit-constructor)
    template <typename F>
        requires std::is_invocable_r_v<T, F> && (!std::is_convertible_v<F, T>)
    Prop(F &&fn) : fn_(std::forward<F>(fn)) {}  // NOLINT(google-explicit-constructor)

    [[nodiscard]] bool reactive() const { return static_cast<bool>(fn_); }
    // Tracked when reactive.
    [[nodiscard]] T get() const { return fn_ ? fn_() : value_; }

private:
    T value_{};
    std::function<T()> fn_;
};

}  // namespace filepan::ui
