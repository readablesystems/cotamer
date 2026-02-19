#include "cotamer.hh"
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace cot = cotamer;
using namespace std::chrono_literals;


cot::task<int> h() {
    co_await cot::after(1h);
    co_return 10;
}

cot::task<> f() {
    co_await cot::all(cot::asap(), cot::after(12h));
    std::cerr << cot::now() << ": f1\n";
    co_await cot::all(cot::asap(), cot::after(24h));
    std::cerr << cot::now() << ": f\n";
}

cot::task<> g() {
    co_await cot::any(cot::asap(), cot::after(24h));
    std::cerr << cot::now() << ": g\n";
    co_await cot::after(1h);
    std::cerr << cot::now() << ": g'\n";
    auto z = co_await h();
    std::cerr << cot::now() << ": " << z << "\n";
}

// 1. Synchronous task completion (no suspension)
cot::task<int> immediate() {
    co_return 42;
}
cot::task<> test_sync() {
    auto start = cot::now();
    auto z = co_await immediate();
    std::cerr << "sync: " << z << "\n";
    assert(z == 42 && start == cot::now());
}

// 2. Exception propagation
cot::task<int> throwing() {
    co_await cot::after(1h);
    throw std::runtime_error("boom");
    co_return 0;
}
cot::task<> test_exception() {
    auto start = cot::now();
    try {
        (void) co_await throwing();
        assert(false && "BUG: should not reach here");
    } catch (const std::runtime_error& e) {
        std::cerr << "caught: " << e.what() << "\n";
    }
    assert(cot::now() - start >= 1h);
}

// 3. Deep task chain
cot::task<int> chain(int n) {
    co_await cot::after(1h);
    if (n > 0) {
        auto v = co_await chain(n - 1);
        co_return v + 1;
    }
    co_return 0;
}
cot::task<> test_chain() {
    auto start = cot::now();
    auto z = co_await chain(5);
    std::cerr << "chain: " << z << "\n";
    assert(z == 5);
    assert(cot::now() - start >= 6h && cot::now() - start < 7h);
}

// 4. any() cleanup — losing event fires later
cot::task<> test_any_cleanup() {
    auto start = cot::now();
    co_await cot::any(cot::after(1h), cot::after(100h));
    std::cerr << cot::now() << ": any_cleanup done\n";
    assert(cot::now() - start >= 1h && cot::now() - start < 2h);
}

// 5. all() with 3 events at different times
cot::task<> test_all_multi() {
    auto start = cot::now();
    co_await cot::all(cot::after(1h), cot::after(2h), cot::after(3h));
    std::cerr << cot::now() << ": all3 done\n";
    assert(cot::now() - start >= 3h && cot::now() - start < 4h);
}

// 6. Nested any/all
cot::task<> test_nested_combinators() {
    auto start = cot::now();
    co_await cot::any(cot::all(cot::after(1h), cot::after(2h)), cot::after(10h));
    std::cerr << cot::now() << ": nested done\n";
    assert(cot::now() - start >= 2h && cot::now() - start < 3h);
}

// 7. co_await a task<void>
cot::task<> inner_void() {
    co_await cot::after(1h);
    std::cerr << cot::now() << ": inner void done\n";
}
cot::task<> test_await_void() {
    auto start = cot::now();
    co_await inner_void();
    std::cerr << cot::now() << ": outer void done\n";
    assert(cot::now() - start >= 1h && cot::now() - start < 2h);
}

// 8. Two racing tasks
cot::task<> racer(const char* name, int hours) {
    co_await cot::after(std::chrono::hours(hours));
    std::cerr << cot::now() << ": " << name << "\n";
}
cot::task<> test_racers() {
    auto start = cot::now();
    racer("A", 3).detach();
    racer("B", 2).detach();
    co_await cot::after(4h);
    std::cerr << cot::now() << ": racers done\n";
    assert(cot::now() - start >= 4h && cot::now() - start < 5h);
}

// 9. any/all with all asap
cot::task<> test_any_all_asap() {
    auto start = cot::now();
    co_await cot::any(cot::asap(), cot::asap());
    std::cerr << "any-asap done\n";
    co_await cot::all(cot::asap(), cot::asap());
    std::cerr << "all-asap done\n";
    assert(cot::now() - start < 1h);
}

// 10. Move-only return type
cot::task<std::unique_ptr<int>> make_ptr() {
    co_await cot::after(1h);
    co_return std::make_unique<int>(99);
}
cot::task<> test_move_only() {
    auto start = cot::now();
    auto p = co_await make_ptr();
    std::cerr << "ptr: " << *p << "\n";
    assert(*p == 99);
    assert(cot::now() - start >= 1h && cot::now() - start < 2h);
}

// 11. any() with no events does not wait
cot::task<> test_any0() {
    auto start = cot::now();
    co_await cot::any();
    std::cerr << "any\n";
    assert(start == cot::now());
}

// 12. attempt succeeds — task completes before timeout
cot::task<int> slow_value() {
    co_await cot::after(1h);
    co_return 77;
}
cot::task<> test_attempt_success() {
    auto start = cot::now();
    auto result = co_await cot::attempt(slow_value(), cot::after(10h));
    assert(result.has_value());
    assert(*result == 77);
    assert(cot::now() - start >= 1h && cot::now() - start < 2h);
    std::cerr << "attempt_success: " << *result << "\n";
}

// 13. attempt cancelled — timeout fires before task completes
cot::task<int> very_slow_value() {
    co_await cot::after(100h);
    co_return 99;
}
cot::task<> test_attempt_cancelled() {
    auto start = cot::now();
    auto result = co_await cot::attempt(very_slow_value(), cot::after(1h));
    assert(!result.has_value());
    assert(cot::now() - start >= 1h && cot::now() - start < 2h);
    std::cerr << "attempt_cancelled: nullopt\n";
}

// 14. attempt with already-done task
cot::task<> test_attempt_already_done() {
    auto start = cot::now();
    auto result = co_await cot::attempt(immediate(), cot::after(10h));
    assert(result.has_value());
    assert(*result == 42);
    assert(cot::now() - start < 1h);
    std::cerr << "attempt_already_done: " << *result << "\n";
}

// 15. attempt with nested tasks — cancellation cascades
cot::task<int> nested_slow() {
    co_await cot::after(50h);
    auto v = co_await very_slow_value();
    co_return v + 1;
}
cot::task<> test_attempt_nested() {
    auto start = cot::now();
    auto result = co_await cot::attempt(nested_slow(), cot::after(1h));
    assert(!result.has_value());
    assert(cot::now() - start >= 1h && cot::now() - start < 2h);
    std::cerr << "attempt_nested: nullopt\n";
}

// 16. attempt on task<optional<T>> — succeeds, no double-wrapping
cot::task<std::optional<int>> maybe_value() {
    co_await cot::after(1h);
    co_return 55;
}
cot::task<> test_attempt_optional_success() {
    auto start = cot::now();
    auto result = co_await cot::attempt(maybe_value(), cot::after(10h));
    // result should be std::optional<int>, not std::optional<std::optional<int>>
    static_assert(std::is_same_v<decltype(result), std::optional<int>>);
    assert(result.has_value());
    assert(*result == 55);
    assert(cot::now() - start >= 1h && cot::now() - start < 2h);
    std::cerr << "attempt_optional_success: " << *result << "\n";
}

// 17. attempt on task<optional<T>> — cancelled
cot::task<> test_attempt_optional_cancelled() {
    auto start = cot::now();
    auto result = co_await cot::attempt(maybe_value(), cot::after(0h));
    static_assert(std::is_same_v<decltype(result), std::optional<int>>);
    assert(!result.has_value());
    assert(cot::now() - start < 1h);
    std::cerr << "attempt_optional_cancelled: nullopt\n";
}

// 18. attempt on task<optional<T>> — task itself returns nullopt
cot::task<std::optional<int>> returns_nullopt() {
    co_await cot::after(1h);
    co_return std::nullopt;
}
cot::task<> test_attempt_optional_inner_nullopt() {
    auto start = cot::now();
    auto result = co_await cot::attempt(returns_nullopt(), cot::after(10h));
    static_assert(std::is_same_v<decltype(result), std::optional<int>>);
    // Task completed but returned nullopt — indistinguishable from cancellation
    assert(!result.has_value());
    assert(cot::now() - start >= 1h && cot::now() - start < 2h);
    std::cerr << "attempt_optional_inner_nullopt: nullopt\n";
}

// 19. Lazy task with co_await — task doesn't run until co_awaited
bool lazy_ran = false;
cot::task<int> lazy_task() {
    co_await cot::interest{};
    lazy_ran = true;
    co_await cot::after(1h);
    co_return 42;
}
cot::task<> test_lazy_await() {
    lazy_ran = false;
    auto start = cot::now();
    auto t = lazy_task();
    co_await cot::after(10h);  // advance virtual time; task still blocked on interest
    assert(!lazy_ran && "lazy task should not have run yet");
    auto val = co_await t;     // triggers interest
    assert(lazy_ran && "lazy task should have run");
    assert(val == 42);
    assert(cot::now() - start >= 11h && cot::now() - start < 12h);
    std::cerr << "lazy_await: " << val << "\n";
}

// 20. Lazy task with start() and completion()
bool lazy_comp_ran = false;
cot::task<int> lazy_comp_task() {
    co_await cot::interest{};
    lazy_comp_ran = true;
    co_await cot::after(1h);
    co_return 77;
}
cot::task<> test_lazy_completion() {
    lazy_comp_ran = false;
    auto start = cot::now();
    auto t = lazy_comp_task();
    co_await cot::after(10h);  // advance virtual time; task still blocked on interest
    assert(!lazy_comp_ran && "lazy task should not have run yet");
    t.start();
    auto ev = t.completion();
    co_await ev;
    assert(lazy_comp_ran && "lazy task should have run after start()");
    auto val = co_await t;
    assert(val == 77);
    assert(cot::now() - start >= 11h && cot::now() - start < 12h);
    std::cerr << "lazy_completion: " << val << "\n";
}

// 21. Lazy task with attempt — attempt triggers interest via completion()
cot::task<int> lazy_attempt_task() {
    co_await cot::interest{};
    co_await cot::after(1h);
    co_return 55;
}
cot::task<> test_lazy_attempt() {
    auto start = cot::now();
    auto result = co_await cot::attempt(lazy_attempt_task(), cot::after(100h));
    assert(result.has_value());
    assert(*result == 55);
    assert(cot::now() - start >= 1h && cot::now() - start < 2h);
    std::cerr << "lazy_attempt: " << *result << "\n";
}

// 22. Lazy task with internal timeout — any(interest, after(...)) auto-starts
bool lazy_timeout_ran = false;
cot::clock::time_point lazy_timeout_time;
cot::task<int> lazy_timeout_task() {
    co_await cot::any(cot::interest{}, cot::after(5h));
    lazy_timeout_ran = true;
    lazy_timeout_time = cot::now();
    co_await cot::after(1h);
    co_return 33;
}
cot::task<> test_lazy_internal_timeout() {
    lazy_timeout_ran = false;
    auto start = cot::now();
    auto t = lazy_timeout_task();
    // Don't co_await t — let the internal timeout fire
    co_await cot::after(10h);
    assert(lazy_timeout_ran && "task should have auto-started via internal timeout");
    // Task body should have started at ~5h (from internal timeout), not 10h
    assert(lazy_timeout_time - start <= 6h && "should have started near the 5h timeout");
    auto val = co_await t;
    assert(val == 33);
    std::cerr << "lazy_internal_timeout: " << val << "\n";
}

// 23. Non-lazy task — verify existing tasks are unaffected
bool nonlazy_ran = false;
cot::task<int> nonlazy_task() {
    nonlazy_ran = true;
    co_await cot::after(1h);
    co_return 99;
}
cot::task<> test_nonlazy() {
    nonlazy_ran = false;
    auto start = cot::now();
    auto t = nonlazy_task();
    assert(nonlazy_ran && "non-lazy task should run immediately");
    auto val = co_await t;
    assert(val == 99);
    assert(cot::now() - start >= 1h && cot::now() - start < 2h);
    std::cerr << "nonlazy: " << val << "\n";
}

// 24. Pre-started task with nested interest quorum — exercises apply_interest
//     delegation through non-f_interest quorum.
//     The task suspends on a non-interest event first, has start() called,
//     then reaches co_await all(any(interest{}, ...), after(...)).
//     apply_interest delegates to the inner any-quorum, which completes
//     immediately (interest is pre-triggered). The outer all-quorum gets
//     one trigger but needs two, so the task waits for after(3h).
cot::task<int> delayed_interest_task() {
    co_await cot::after(1h);
    co_await cot::all(
        cot::any(cot::interest{}, cot::after(100h)),
        cot::after(3h)
    );
    co_return 42;
}
cot::task<> test_delayed_interest() {
    auto start = cot::now();
    auto t = delayed_interest_task();
    // Task is suspended on after(1h)
    t.start();  // pre-trigger interest (no event yet, sets has_interest_)
    auto val = co_await t;
    assert(val == 42);
    // Should complete at 4h (1h + 3h), NOT 101h (1h + 100h)
    assert(cot::now() - start >= 4h && cot::now() - start < 5h);
    std::cerr << "delayed_interest: " << val << "\n";
}

// 25. Pre-started task with top-level any(interest{}, ...) — the cascade
//     fully satisfies the any-quorum during apply_interest, so the event
//     is already triggered when await_suspend tries to add the coroutine.
cot::task<int> prestarted_any_task() {
    co_await cot::after(1h);
    co_await cot::any(cot::interest{}, cot::after(100h));
    co_return 42;
}
cot::task<> test_prestarted_any() {
    auto start = cot::now();
    auto t = prestarted_any_task();
    t.start();
    auto val = co_await t;
    assert(val == 42);
    assert(cot::now() - start >= 1h && cot::now() - start < 2h);
    std::cerr << "prestarted_any: " << val << "\n";
}

// 26. Folded interest plus nested interest child — exercises the sequential
//     (non-else) path in fix_want_interest, where the quorum has both
//     f_interest (from a direct interest{} argument) and a child with
//     f_want_interest (from any(interest{}, ...)).
cot::task<int> interest_and_nested_interest() {
    co_await cot::all(cot::interest{}, cot::any(cot::interest{}, cot::after(100h)));
    co_return 42;
}
cot::task<> test_interest_and_nested() {
    auto start = cot::now();
    auto t = interest_and_nested_interest();
    auto val = co_await t;  // triggers interest
    assert(val == 42);
    // Interest satisfies both the folded interest{} and the nested
    // any(interest{}, ...), so the all-quorum completes without waiting
    // for after(100h).
    assert(cot::now() - start < 1h);
    std::cerr << "interest_and_nested: " << val << "\n";
}

// 27. interest_event — returns event without suspending
bool ie_continued = false;
cot::task<int> interest_event_task() {
    auto e = co_await cot::interest_event{};
    ie_continued = true;  // should run immediately, no suspend
    assert(!e.triggered() && "interest event should not be triggered yet");
    co_await e;  // now actually wait for interest
    assert(e.triggered());
    co_await cot::after(1h);
    co_return 42;
}
cot::task<> test_interest_event() {
    ie_continued = false;
    auto start = cot::now();
    auto t = interest_event_task();
    assert(ie_continued && "interest_event{} should not suspend");
    co_await cot::after(10h);
    assert(!t.done() && "task should still be waiting for interest");
    auto val = co_await t;  // triggers interest
    assert(val == 42);
    assert(cot::now() - start >= 11h && cot::now() - start < 12h);
    std::cerr << "interest_event: " << val << "\n";
}

// 28. explicit_trigger_quorum — test difference between natural and explicit
// trigger on a quorum event
cot::task<> test_explicit_trigger_quorum() {
    // Natural case: quorum completes because one member fires
    {
        auto slow = cot::after(100h);
        auto q = cot::any(cot::after(1h), slow);
        // slow has the quorum as a listener
        assert(!slow.idle());
        co_await q;  // after(1h) fires → complete_quorum deregisters from slow
        assert(slow.idle() && "natural: slow should have no listeners after quorum completes");
    }

    // Explicit trigger case: quorum triggered directly
    {
        auto slow = cot::after(100h);
        auto q = cot::any(cot::after(100h), slow);
        // slow has the quorum as a listener
        assert(!slow.idle());
        q.trigger();  // trigger quorum explicitly
        co_await q;   // resumes immediately (already triggered)
        // members_ not cleared → slow still has quorum as listener
        assert(slow.idle() && "explicit: slow should have no listeners after quorum completes");
    }
    std::cerr << "explicit_trigger_quorum: ok\n";
}

// 29. multi_interest — all(interest{}, interest{}) should count both
cot::task<int> multi_interest_task() {
    co_await cot::all(cot::interest{}, cot::interest{});
    co_return 42;
}
cot::task<> test_multi_interest() {
    auto start = cot::now();
    auto t = multi_interest_task();
    auto val = co_await t;  // triggers interest, satisfying both interest{} members
    assert(val == 42);
    assert(cot::now() - start < 1h);
    std::cerr << "multi_interest: " << val << "\n";
}

// 30. Nested interest propagation through any()
//     In fix_want_interest, if processing local interest{} satisfies the quorum
//     (as it does for any()), we trigger and return WITHOUT propagating interest
//     to child quorums. This is the lazy semantics: interest propagates only
//     when someone actually co_awaits the child event, not eagerly through the
//     quorum chain. This test verifies that behavior.
cot::task<int> nip_task() {
    co_await cot::interest{};   // suspend until someone co_awaits us
    // Now our interest event IS triggered.

    // inner: would resolve immediately if interest were propagated
    auto inner = cot::any(cot::interest{}, cot::after(100h));

    // Outer: any(interest{}, inner). Our interest is already triggered,
    // so fix_want_interest's interest{} satisfies the any immediately
    // without propagating interest to inner.
    co_await cot::any(cot::interest{}, inner);

    // inner was NOT triggered (interest not propagated).
    assert(!inner.triggered());

    // But a re-await resolves inner immediately: fix_want_interest fires
    // with our (still-triggered) interest event.
    co_await inner;
    assert(inner.triggered());

    co_return 42;
}
cot::task<> test_nested_interest_propagation() {
    auto start = cot::now();
    auto t = nip_task();
    auto val = co_await t;
    assert(val == 42);
    assert(cot::now() - start < 1h);
    std::cerr << "nested_interest_propagation: " << val << "\n";
}

// 31. Stored event triggers without listeners — tests that the timer heap
//     does not garbage-collect an event just because nobody is co_awaiting it.
//     Create an event via after(200ms), store it in a variable (but don't
//     co_await it), then wait 400ms. The stored event should have triggered.
cot::task<> test_stored_event_triggers() {
    auto start = cot::now();
    auto e = cot::after(200ms);
    assert(!e.triggered() && "event should not be triggered immediately");
    co_await cot::after(400ms);
    assert(e.triggered() && "stored event should have triggered after 400ms");
    assert(cot::now() - start >= 400ms && cot::now() - start < 500ms);
    std::cerr << "stored_event_triggers: ok\n";
}

// 32. Timer heap culling — verifies that empty() allows the timer heap to
//     stay small. Create many any(after(10ms), after(300ms)) events. After
//     the 10ms timers fire, the any() quorums complete and deregister from the
//     300ms timers, making those entries cullable. After 100ms the timer heap
//     should be much smaller than the number of 300ms entries we created.
cot::task<> test_timer_heap_cull() {
    std::vector<cot::event> events;
    for (int i = 0; i < 100; ++i) {
        events.push_back(cot::any(cot::after(10ms), cot::after(300ms)));
    }
    // 200 timer entries created (100 x 10ms + 100 x 300ms), plus our own
    co_await cot::after(100ms);
    // Yield so the event loop runs cull() on the timer heap (cull runs
    // after processing ready coroutines, before the next time step).
    co_await cot::asap();
    // The 10ms timers have fired and been popped. The 300ms timer entries
    // should have been culled (idle, single reference).
    size_t sz = cot::driver::main->timer_size();
    std::cerr << "timer_heap_cull: timer_size=" << sz << "\n";
    assert(sz < 10 && "timer heap should have culled stale 300ms entries");
}

// 33. any(event()) should not be immediately triggered.
//     A default-constructed event is untriggered, and any() of an untriggered
//     event should remain untriggered. Test both the single-argument path
//     (make_event passthrough) and the multi-argument quorum path.
cot::task<> test_any_default_event() {
    auto e1 = cot::any(cot::event());
    assert(!e1.triggered() && "any(event()) should not be immediately triggered");
    auto e2 = cot::any(cot::event(), cot::event());
    assert(!e2.triggered() && "any(event(), event()) should not be immediately triggered");
    std::cerr << "any_default_event: ok\n";
    co_return;
}

// 35. Duplicate event in quorum — the same event added to a quorum multiple
//     times. When it triggers, trigger_member is called once per listener entry,
//     and the quorum must handle the duplicate removals without double-free.
cot::task<> test_duplicate_event_in_quorum() {
    // Case 1: any(e, e, e2) — e triggers, quorum fires on first trigger_member,
    // second trigger_member must safely remove the remaining duplicate.
    {
        auto e = cot::event();
        auto e2 = cot::event();
        auto q = cot::any(e, e, e2);
        assert(!q.triggered());
        e.trigger();
        assert(q.triggered() && "any(e,e,e2) should trigger when e triggers");
    }

    // Case 2: the user's exact scenario — nested in all()
    {
        auto e = cot::event();
        auto e2 = cot::event();
        auto q = cot::all(cot::any(e, e, e2), cot::after(200ms));
        assert(!q.triggered());
        e.trigger();
        // any() is satisfied but all() still needs after(200ms)
        assert(!q.triggered() && "all() should not trigger yet");
        co_await q;
        assert(q.triggered());
    }

    // Case 3: all(e, e) — same event twice in an all-quorum.
    // Triggering e once should count for both copies.
    {
        auto e = cot::event();
        auto q = cot::all(e, e);
        assert(!q.triggered());
        e.trigger();
        assert(q.triggered() && "all(e,e) should trigger when e triggers");
    }

    // Case 4: any(e, e) — dropped without triggering.
    // Tests the destroy/cull path with duplicate untriggered members.
    {
        auto e = cot::event();
        auto q = cot::any(e, e);
        assert(!q.triggered());
        // q goes out of scope → destroy() → cull_members() with duplicates
    }

    // Case 5: any(e, e) with explicit trigger on e
    {
        auto e = cot::event();
        auto q = cot::any(e, e);
        e.trigger();
        assert(q.triggered());
        co_await q; // should resume immediately
    }

    // Case 6: three copies in any(), nested in all() with another any()
    {
        auto e = cot::event();
        auto e2 = cot::event();
        auto q = cot::all(cot::any(e, e, e), cot::any(e2, e2));
        assert(!q.triggered());
        e.trigger();
        assert(!q.triggered() && "need e2 too for all()");
        e2.trigger();
        assert(q.triggered());
    }

    std::cerr << "duplicate_event_in_quorum: ok\n";
    co_return;
}

int main(int argc, char* argv[]) {
    unsigned ran = 0;

    auto run = [&](const char* name, auto fn) {
        bool found = argc == 1;
        for (int argi = 1; !found && argi < argc; ++argi) {
            found = strcmp(name, argv[argi]) == 0;
        }
        if (!found) {
            return;
        }
        ++ran;
        std::cerr << "=== " << name << " ===\n";
        cot::reset();
        auto t = fn();
        cot::loop();
        assert(t.done() && "test did not complete");
    };

    run("original", []() -> cot::task<> {
        f().detach();
        g().detach();
        co_return;
    });
    run("sync", test_sync);
    run("exception", test_exception);
    run("chain", test_chain);
    run("any_cleanup", test_any_cleanup);
    run("all_multi", test_all_multi);
    run("nested", test_nested_combinators);
    run("await_void", test_await_void);
    run("racers", test_racers);
    run("any_all_asap", test_any_all_asap);
    run("move_only", test_move_only);
    run("any0", test_any0);
    run("attempt_success", test_attempt_success);
    run("attempt_cancelled", test_attempt_cancelled);
    run("attempt_already_done", test_attempt_already_done);
    run("attempt_nested", test_attempt_nested);
    run("attempt_optional_success", test_attempt_optional_success);
    run("attempt_optional_cancelled", test_attempt_optional_cancelled);
    run("attempt_optional_inner_nullopt", test_attempt_optional_inner_nullopt);
    run("lazy_await", test_lazy_await);
    run("lazy_completion", test_lazy_completion);
    run("lazy_attempt", test_lazy_attempt);
    run("lazy_internal_timeout", test_lazy_internal_timeout);
    run("nonlazy", test_nonlazy);
    run("delayed_interest", test_delayed_interest);
    run("prestarted_any", test_prestarted_any);
    run("interest_and_nested", test_interest_and_nested);
    run("interest_event", test_interest_event);
    run("explicit_trigger_quorum", test_explicit_trigger_quorum);
    run("multi_interest", test_multi_interest);
    run("nested_interest_propagation", test_nested_interest_propagation);
    run("stored_event_triggers", test_stored_event_triggers);
    run("timer_heap_cull", test_timer_heap_cull);
    run("any_default_event", test_any_default_event);
    run("duplicate_event_in_quorum", test_duplicate_event_in_quorum);

    if (ran == 0) {
        std::print(std::cerr, "No matching tests\n");
        exit(1);
    } else {
        std::print(std::cerr, "*** done ***\n");
        exit(0);
    }
}
