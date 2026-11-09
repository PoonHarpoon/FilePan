// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include <string>
#include <vector>

#include "filepan/ui/reactive.hpp"
#include "test_util.hpp"

using namespace filepan::ui;

namespace {

void testSignalAndEffect() {
    Scope scope;
    Signal<int> count(1);
    std::vector<int> seen;
    scope.run([&] { effect([&] { seen.push_back(count()); }); });
    EXPECT(seen == std::vector<int>{1});
    count.set(2);
    EXPECT(seen == (std::vector<int>{1, 2}));
    count.set(2);  // Equal: no re-run.
    EXPECT(seen.size() == 2);
    count.update([](int &value) { value += 3; });
    EXPECT(seen.back() == 5);
}

void testMemoIsLazyAndCutsOff() {
    Scope scope;
    Signal<int> value(3);
    int memoRuns = 0;
    int effectRuns = 0;
    scope.run([&] {
        Memo<bool> positive([&] {
            ++memoRuns;
            return value() > 0;
        });
        effect([&, positive] {
            (void)positive();
            ++effectRuns;
        });
    });
    EXPECT(memoRuns == 1 && effectRuns == 1);
    value.set(7);  // Still positive: the memo re-runs, the effect doesn't.
    EXPECT(memoRuns == 2 && effectRuns == 1);
    value.set(-1);
    EXPECT(memoRuns == 3 && effectRuns == 2);
}

void testGlitchFreeDiamond() {
    Scope scope;
    Signal<int> a(1);
    std::vector<std::string> seen;
    scope.run([&] {
        Memo<int> b([a] { return a() * 2; });
        Memo<int> c([a] { return a() * 3; });
        Memo<int> d([b, c] { return b() + c(); });
        effect([&, a, d] { seen.push_back(std::to_string(a()) + ":" + std::to_string(d())); });
    });
    a.set(2);
    // Never an inconsistent "2:7" between the two memos updating.
    EXPECT(seen == (std::vector<std::string>{"1:5", "2:10"}));
}

void testBatch() {
    Scope scope;
    Signal<int> a(0);
    Signal<int> b(0);
    int runs = 0;
    scope.run([&] {
        effect([&] {
            (void)a();
            (void)b();
            ++runs;
        });
    });
    batch([&] {
        a.set(1);
        b.set(1);
        EXPECT(runs == 1);
    });
    EXPECT(runs == 2);
}

void testDynamicDependencies() {
    Scope scope;
    Signal<bool> useA(true);
    Signal<int> a(1);
    Signal<int> b(10);
    int last = 0;
    int runs = 0;
    scope.run([&] {
        effect([&] {
            ++runs;
            last = useA() ? a() : b();
        });
    });
    b.set(11);  // Not a dependency yet.
    EXPECT(runs == 1);
    useA.set(false);
    EXPECT(last == 11 && runs == 2);
    a.set(2);  // No longer a dependency.
    EXPECT(runs == 2);
    b.set(12);
    EXPECT(last == 12 && runs == 3);
}

void testDisposalAndCleanup() {
    Signal<int> value(0);
    int runs = 0;
    int cleanups = 0;
    {
        Scope scope;
        scope.run([&] {
            effect([&] {
                (void)value();
                ++runs;
                onCleanup([&] { ++cleanups; });
            });
        });
        value.set(1);
        EXPECT(runs == 2 && cleanups == 1);
    }
    EXPECT(cleanups == 2);
    value.set(2);
    EXPECT(runs == 2);
}

void testNestedEffectsDisposedOnRerun() {
    Scope scope;
    Signal<int> outer(0);
    Signal<int> inner(0);
    int innerRuns = 0;
    scope.run([&] {
        effect([&] {
            (void)outer();
            effect([&] {
                (void)inner();
                ++innerRuns;
            });
        });
    });
    EXPECT(innerRuns == 1);
    outer.set(1);  // Disposes the old inner effect and creates a new one.
    EXPECT(innerRuns == 2);
    inner.set(1);  // Only one inner effect is alive.
    EXPECT(innerRuns == 3);
}

void testScheduler() {
    auto &rt = filepan::ui::detail::runtime();
    int requests = 0;
    rt.scheduler = [&] { ++requests; };
    Scope scope;
    Signal<int> value(0);
    int seen = -1;
    scope.run([&] { effect([&] { seen = value(); }); });
    value.set(1);
    value.set(2);
    EXPECT(seen == 0 && requests == 1);
    flushEffects();
    EXPECT(seen == 2);
    rt.scheduler = nullptr;
}

void testUntrack() {
    Scope scope;
    Signal<int> tracked(0);
    Signal<int> ignored(0);
    int runs = 0;
    scope.run([&] {
        effect([&] {
            (void)tracked();
            untrack([&] { return ignored(); });
            ++runs;
        });
    });
    ignored.set(1);
    EXPECT(runs == 1);
    tracked.set(1);
    EXPECT(runs == 2);
}

void testSignalDestroyedBeforeEffect() {
    Scope scope;
    int runs = 0;
    {
        Signal<int> shortLived(0);
        scope.run([&] {
            effect([&, shortLived] {
                (void)shortLived();
                ++runs;
            });
        });
    }
    scope.dispose();  // Releases the captured signal last; must not touch freed memory.
    EXPECT(runs == 1);
}

void testProp() {
    const Prop<std::string> literal = "Delete";
    EXPECT(!literal.reactive() && literal.get() == "Delete");
    Signal<int> count(2);
    const Prop<std::string> bound = [count] { return std::to_string(count()) + " files"; };
    EXPECT(bound.reactive() && bound.get() == "2 files");
    count.set(3);
    EXPECT(bound.get() == "3 files");
    const Prop<float> converted = 1;
    EXPECT(converted.get() == 1.0f);
}

}  // namespace

int main() {
    testSignalAndEffect();
    testMemoIsLazyAndCutsOff();
    testGlitchFreeDiamond();
    testBatch();
    testDynamicDependencies();
    testDisposalAndCleanup();
    testNestedEffectsDisposedOnRerun();
    testScheduler();
    testUntrack();
    testSignalDestroyedBeforeEffect();
    testProp();
    return 0;
}
