#include "cotamer.hh"
#include <atomic>
#include <cassert>
#include <iostream>
#include <optional>
#include <thread>
#include <vector>

namespace cot = cotamer;


// Test 1: Cross-thread trigger — thread B triggers an event owned by thread A,
// coroutine resumes on thread A.
void test_cross_thread_trigger() {
    std::cerr << "=== cross_thread_trigger ===\n";

    std::atomic<int> phase{0};
    std::thread::id resumed_thread;
    cot::event* ev_ptr = nullptr;

    std::thread thread_a([&] {
        cot::event ev;
        ev_ptr = &ev;

        auto fn = [&]() -> cot::task<> {
            co_await ev;
            resumed_thread = std::this_thread::get_id();
        };
        auto t = fn();

        phase.store(1, std::memory_order_release);

        while (phase.load(std::memory_order_acquire) < 2) {
            std::this_thread::yield();
        }

        cot::loop();

        assert(t.done() && "coroutine should have completed");
        assert(resumed_thread == std::this_thread::get_id()
               && "coroutine should have resumed on thread A");
    });

    std::thread thread_b([&] {
        while (phase.load(std::memory_order_acquire) < 1) {
            std::this_thread::yield();
        }

        ev_ptr->trigger();

        phase.store(2, std::memory_order_release);
    });

    thread_a.join();
    thread_b.join();

    std::cerr << "cross_thread_trigger: ok\n";
}


// Test 2: Cross-thread trigger through an any() combinator.
void test_cross_thread_any() {
    std::cerr << "=== cross_thread_any ===\n";

    std::atomic<int> phase{0};
    std::thread::id resumed_thread;
    cot::event* ev_ptr = nullptr;

    std::thread thread_a([&] {
        cot::event ev;
        ev_ptr = &ev;

        auto fn = [&]() -> cot::task<> {
            co_await cot::any(ev, cot::after(std::chrono::hours(1000)));
            resumed_thread = std::this_thread::get_id();
        };
        auto t = fn();

        phase.store(1, std::memory_order_release);

        while (phase.load(std::memory_order_acquire) < 2) {
            std::this_thread::yield();
        }

        cot::loop();

        assert(t.done() && "coroutine should have completed");
        assert(resumed_thread == std::this_thread::get_id()
               && "coroutine should have resumed on thread A");
    });

    std::thread thread_b([&] {
        while (phase.load(std::memory_order_acquire) < 1) {
            std::this_thread::yield();
        }

        ev_ptr->trigger();

        phase.store(2, std::memory_order_release);
    });

    thread_a.join();
    thread_b.join();

    std::cerr << "cross_thread_any: ok\n";
}


// Test 3: Multiple cross-thread triggers — thread B triggers several events.
void test_cross_thread_multi() {
    std::cerr << "=== cross_thread_multi ===\n";

    constexpr int N = 10;
    std::atomic<int> phase{0};
    int resume_count = 0;
    std::thread::id a_id;
    cot::event* evs[N];

    std::thread thread_a([&] {
        a_id = std::this_thread::get_id();
        cot::event events[N];
        std::optional<cot::task<>> tasks[N];

        auto fn = [&](int i) -> cot::task<> {
            co_await events[i];
            assert(std::this_thread::get_id() == a_id);
            ++resume_count;
        };
        for (int i = 0; i < N; ++i) {
            evs[i] = &events[i];
            tasks[i] = fn(i);
        }

        phase.store(1, std::memory_order_release);

        while (phase.load(std::memory_order_acquire) < 2) {
            std::this_thread::yield();
        }

        cot::loop();

        for (int i = 0; i < N; ++i) {
            assert(tasks[i]->done());
        }
        assert(resume_count == N);
    });

    std::thread thread_b([&] {
        while (phase.load(std::memory_order_acquire) < 1) {
            std::this_thread::yield();
        }

        for (int i = 0; i < N; ++i) {
            evs[i]->trigger();
        }

        phase.store(2, std::memory_order_release);
    });

    thread_a.join();
    thread_b.join();

    std::cerr << "cross_thread_multi: ok\n";
}


// Test 4: Each thread runs its own independent driver.
void test_independent_drivers() {
    std::cerr << "=== independent_drivers ===\n";

    std::atomic<int> done_count{0};

    auto thread_fn = [&] {
        cot::event ev;
        int result = 0;

        auto fn = [&]() -> cot::task<> {
            co_await ev;
            result = 42;
        };
        auto t = fn();

        ev.trigger();
        cot::loop();

        assert(t.done());
        assert(result == 42);
        done_count.fetch_add(1);
    };

    std::thread t1(thread_fn);
    std::thread t2(thread_fn);
    std::thread t3(thread_fn);

    t1.join();
    t2.join();
    t3.join();

    assert(done_count.load() == 3);
    std::cerr << "independent_drivers: ok\n";
}


// Test 5: Cross-thread trigger on an already-triggered event is a no-op.
void test_cross_thread_already_triggered() {
    std::cerr << "=== cross_thread_already_triggered ===\n";

    std::atomic<int> phase{0};
    cot::event* ev_ptr = nullptr;

    std::thread thread_a([&] {
        cot::event ev;
        ev_ptr = &ev;

        ev.trigger();
        assert(ev.triggered());

        phase.store(1, std::memory_order_release);

        while (phase.load(std::memory_order_acquire) < 2) {
            std::this_thread::yield();
        }

        cot::loop();
    });

    std::thread thread_b([&] {
        while (phase.load(std::memory_order_acquire) < 1) {
            std::this_thread::yield();
        }

        ev_ptr->trigger();

        phase.store(2, std::memory_order_release);
    });

    thread_a.join();
    thread_b.join();

    std::cerr << "cross_thread_already_triggered: ok\n";
}


// --- Torture tests below ---


// Torture 1: 8 threads all triggering events on one driver, many rounds.
// Exercises high contention on the driver lock (remote_ready_).
void test_torture_hammer() {
    std::cerr << "=== torture_hammer ===\n";

    constexpr int NTHREADS = 8;
    constexpr int ROUNDS = 100;
    constexpr int EVENTS_PER_ROUND = 50;

    for (int round = 0; round < ROUNDS; ++round) {
        std::atomic<bool> go{false};
        std::atomic<int> done_count{0};
        cot::event* ev_ptrs[EVENTS_PER_ROUND];
        std::atomic<int> resume_count{0};

        std::thread driver_thread([&] {
            cot::event events[EVENTS_PER_ROUND];
            std::optional<cot::task<>> tasks[EVENTS_PER_ROUND];

            auto fn = [&](int i) -> cot::task<> {
                co_await events[i];
                resume_count.fetch_add(1, std::memory_order_relaxed);
            };

            for (int i = 0; i < EVENTS_PER_ROUND; ++i) {
                ev_ptrs[i] = &events[i];
                tasks[i] = fn(i);
            }

            go.store(true, std::memory_order_release);

            // Wait for all triggers to finish
            while (done_count.load(std::memory_order_acquire) < NTHREADS) {
                std::this_thread::yield();
            }

            cot::loop();

            assert(resume_count.load() == EVENTS_PER_ROUND);
            for (int i = 0; i < EVENTS_PER_ROUND; ++i) {
                assert(tasks[i]->done());
            }
        });

        std::vector<std::thread> triggers;
        for (int t = 0; t < NTHREADS; ++t) {
            triggers.emplace_back([&, t] {
                while (!go.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                for (int i = t; i < EVENTS_PER_ROUND; i += NTHREADS) {
                    ev_ptrs[i]->trigger();
                }
                done_count.fetch_add(1, std::memory_order_release);
            });
        }

        driver_thread.join();
        for (auto& t : triggers) {
            t.join();
        }
    }

    std::cerr << "torture_hammer: ok\n";
}


// Torture 2: Multiple threads race to trigger the same event.
// Only one trigger has any effect; the rest hit the lock and see f_triggered.
void test_torture_same_event() {
    std::cerr << "=== torture_same_event ===\n";

    constexpr int NTHREADS = 8;
    constexpr int ROUNDS = 500;

    for (int round = 0; round < ROUNDS; ++round) {
        std::atomic<bool> go{false};
        std::atomic<int> done_count{0};
        cot::event* ev_ptr = nullptr;
        int resume_count = 0;

        std::thread driver_thread([&] {
            cot::event ev;
            ev_ptr = &ev;

            auto fn = [&]() -> cot::task<> {
                co_await ev;
                ++resume_count;
            };
            auto t = fn();

            go.store(true, std::memory_order_release);

            while (done_count.load(std::memory_order_acquire) < NTHREADS) {
                std::this_thread::yield();
            }

            cot::loop();

            assert(t.done());
            assert(resume_count == 1);
        });

        std::vector<std::thread> triggers;
        for (int t = 0; t < NTHREADS; ++t) {
            triggers.emplace_back([&] {
                while (!go.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                ev_ptr->trigger();
                done_count.fetch_add(1, std::memory_order_release);
            });
        }

        driver_thread.join();
        for (auto& t : triggers) {
            t.join();
        }
    }

    std::cerr << "torture_same_event: ok\n";
}


// Torture 3: Each member of an all() quorum triggered from a different thread.
// Exercises trigger_member contention — multiple trigger_members racing on the
// quorum's lock, with exactly one reaching the quorum threshold.
void test_torture_quorum_race() {
    std::cerr << "=== torture_quorum_race ===\n";

    constexpr int NTHREADS = 4;
    constexpr int ROUNDS = 200;

    for (int round = 0; round < ROUNDS; ++round) {
        std::atomic<bool> go{false};
        std::atomic<int> done_count{0};
        cot::event* ev_ptrs[NTHREADS];
        bool completed = false;

        std::thread driver_thread([&] {
            cot::event events[NTHREADS];
            for (int i = 0; i < NTHREADS; ++i) {
                ev_ptrs[i] = &events[i];
            }

            auto fn = [&]() -> cot::task<> {
                co_await cot::all(events[0], events[1], events[2], events[3]);
                completed = true;
            };
            auto t = fn();

            go.store(true, std::memory_order_release);

            while (done_count.load(std::memory_order_acquire) < NTHREADS) {
                std::this_thread::yield();
            }

            cot::loop();

            assert(t.done());
            assert(completed);
        });

        std::vector<std::thread> triggers;
        for (int i = 0; i < NTHREADS; ++i) {
            triggers.emplace_back([&, i] {
                while (!go.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                ev_ptrs[i]->trigger();
                done_count.fetch_add(1, std::memory_order_release);
            });
        }

        driver_thread.join();
        for (auto& t : triggers) {
            t.join();
        }
    }

    std::cerr << "torture_quorum_race: ok\n";
}


// Torture 4: Nested quorums with cross-thread triggers.
// any(all(e0, e1), all(e2, e3)) — four threads each trigger one event.
// The inner all() that completes first satisfies the outer any().
void test_torture_nested_quorum() {
    std::cerr << "=== torture_nested_quorum ===\n";

    constexpr int ROUNDS = 200;

    for (int round = 0; round < ROUNDS; ++round) {
        std::atomic<bool> go{false};
        std::atomic<int> done_count{0};
        cot::event* ev_ptrs[4];
        bool completed = false;

        std::thread driver_thread([&] {
            cot::event e0, e1, e2, e3;
            ev_ptrs[0] = &e0;
            ev_ptrs[1] = &e1;
            ev_ptrs[2] = &e2;
            ev_ptrs[3] = &e3;

            auto fn = [&]() -> cot::task<> {
                co_await cot::any(cot::all(e0, e1), cot::all(e2, e3));
                completed = true;
            };
            auto t = fn();

            go.store(true, std::memory_order_release);

            while (done_count.load(std::memory_order_acquire) < 4) {
                std::this_thread::yield();
            }

            cot::loop();

            assert(t.done());
            assert(completed);
        });

        std::vector<std::thread> triggers;
        for (int i = 0; i < 4; ++i) {
            triggers.emplace_back([&, i] {
                while (!go.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                ev_ptrs[i]->trigger();
                done_count.fetch_add(1, std::memory_order_release);
            });
        }

        driver_thread.join();
        for (auto& t : triggers) {
            t.join();
        }
    }

    std::cerr << "torture_nested_quorum: ok\n";
}


// Torture 5: Bidirectional — two drivers, each triggers events on the other.
// Both loop concurrently, processing injected triggers.
void test_torture_bidirectional() {
    std::cerr << "=== torture_bidirectional ===\n";

    constexpr int ROUNDS = 200;
    constexpr int N = 10;

    for (int round = 0; round < ROUNDS; ++round) {
        std::atomic<int> phase{0};
        cot::event* a_evs[N];
        cot::event* b_evs[N];
        std::atomic<int> a_count{0};
        std::atomic<int> b_count{0};

        std::thread thread_a([&] {
            cot::event events[N];
            std::optional<cot::task<>> tasks[N];

            auto fn = [&](int i) -> cot::task<> {
                co_await events[i];
                a_count.fetch_add(1, std::memory_order_relaxed);
            };

            for (int i = 0; i < N; ++i) {
                a_evs[i] = &events[i];
                tasks[i] = fn(i);
            }

            // Wait for thread B to be ready too
            phase.fetch_add(1, std::memory_order_release);
            while (phase.load(std::memory_order_acquire) < 2) {
                std::this_thread::yield();
            }

            // Trigger B's events
            for (int i = 0; i < N; ++i) {
                b_evs[i]->trigger();
            }

            // Loop until our coroutines are all done
            while (a_count.load(std::memory_order_relaxed) < N) {
                cot::loop();
                std::this_thread::yield();
            }
            cot::loop();

            for (int i = 0; i < N; ++i) {
                assert(tasks[i]->done());
            }
        });

        std::thread thread_b([&] {
            cot::event events[N];
            std::optional<cot::task<>> tasks[N];

            auto fn = [&](int i) -> cot::task<> {
                co_await events[i];
                b_count.fetch_add(1, std::memory_order_relaxed);
            };

            for (int i = 0; i < N; ++i) {
                b_evs[i] = &events[i];
                tasks[i] = fn(i);
            }

            // Wait for thread A to be ready too
            phase.fetch_add(1, std::memory_order_release);
            while (phase.load(std::memory_order_acquire) < 2) {
                std::this_thread::yield();
            }

            // Trigger A's events
            for (int i = 0; i < N; ++i) {
                a_evs[i]->trigger();
            }

            // Loop until our coroutines are all done
            while (b_count.load(std::memory_order_relaxed) < N) {
                cot::loop();
                std::this_thread::yield();
            }
            cot::loop();

            for (int i = 0; i < N; ++i) {
                assert(tasks[i]->done());
            }
        });

        thread_a.join();
        thread_b.join();

        assert(a_count.load() == N);
        assert(b_count.load() == N);
    }

    std::cerr << "torture_bidirectional: ok\n";
}


// Torture 6: Triggers arrive while loop() is actively processing.
// The driver thread starts looping immediately; trigger threads fire events
// with random delays so injections arrive mid-loop.
void test_torture_inject_during_loop() {
    std::cerr << "=== torture_inject_during_loop ===\n";

    constexpr int NTHREADS = 4;
    constexpr int ROUNDS = 100;
    constexpr int EVENTS_PER_ROUND = 40;

    for (int round = 0; round < ROUNDS; ++round) {
        std::atomic<bool> go{false};
        cot::event* ev_ptrs[EVENTS_PER_ROUND];
        std::atomic<int> resume_count{0};

        std::thread driver_thread([&] {
            cot::event events[EVENTS_PER_ROUND];
            std::optional<cot::task<>> tasks[EVENTS_PER_ROUND];

            auto fn = [&](int i) -> cot::task<> {
                co_await events[i];
                resume_count.fetch_add(1, std::memory_order_relaxed);
            };

            for (int i = 0; i < EVENTS_PER_ROUND; ++i) {
                ev_ptrs[i] = &events[i];
                tasks[i] = fn(i);
            }

            go.store(true, std::memory_order_release);

            // Loop repeatedly — triggers arrive concurrently
            while (resume_count.load(std::memory_order_relaxed) < EVENTS_PER_ROUND) {
                cot::loop();
                std::this_thread::yield();
            }
            cot::loop();

            for (int i = 0; i < EVENTS_PER_ROUND; ++i) {
                assert(tasks[i]->done());
            }
        });

        std::vector<std::thread> triggers;
        for (int t = 0; t < NTHREADS; ++t) {
            triggers.emplace_back([&, t] {
                while (!go.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                for (int i = t; i < EVENTS_PER_ROUND; i += NTHREADS) {
                    // Stagger triggers so they interleave with loop()
                    if (i % 3 == 0) {
                        std::this_thread::yield();
                    }
                    ev_ptrs[i]->trigger();
                }
            });
        }

        driver_thread.join();
        for (auto& t : triggers) {
            t.join();
        }
    }

    std::cerr << "torture_inject_during_loop: ok\n";
}


// Torture 7: any() quorum with many members, all triggered from different
// threads simultaneously. Only the first trigger satisfies the quorum;
// the rest race into trigger_member on an already-triggered quorum.
void test_torture_any_race() {
    std::cerr << "=== torture_any_race ===\n";

    constexpr int NTHREADS = 8;
    constexpr int ROUNDS = 200;

    for (int round = 0; round < ROUNDS; ++round) {
        std::atomic<bool> go{false};
        std::atomic<int> done_count{0};
        cot::event* ev_ptrs[NTHREADS];
        bool completed = false;

        std::thread driver_thread([&] {
            cot::event events[NTHREADS];
            for (int i = 0; i < NTHREADS; ++i) {
                ev_ptrs[i] = &events[i];
            }

            auto fn = [&]() -> cot::task<> {
                co_await cot::any(events[0], events[1], events[2], events[3],
                                  events[4], events[5], events[6], events[7]);
                completed = true;
            };
            auto t = fn();

            go.store(true, std::memory_order_release);

            while (done_count.load(std::memory_order_acquire) < NTHREADS) {
                std::this_thread::yield();
            }

            cot::loop();

            assert(t.done());
            assert(completed);
        });

        std::vector<std::thread> triggers;
        for (int i = 0; i < NTHREADS; ++i) {
            triggers.emplace_back([&, i] {
                while (!go.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                ev_ptrs[i]->trigger();
                done_count.fetch_add(1, std::memory_order_release);
            });
        }

        driver_thread.join();
        for (auto& t : triggers) {
            t.join();
        }
    }

    std::cerr << "torture_any_race: ok\n";
}


// Leak test 1: any() frees non-triggering members when the event handle drops.
void test_leak_any() {
    std::cerr << "=== leak_any ===\n";

    cot::event e0, e1, e2;
    auto* b0 = e0.handle().get();
    auto* b1 = e1.handle().get();
    auto* b2 = e2.handle().get();
    assert(b0->refcount_.load() == 1);
    assert(b1->refcount_.load() == 1);
    assert(b2->refcount_.load() == 1);

    {
        // any(e0, e1, e2) — quorum holds handles to all three
        cot::event q = cot::any(e0, e1, e2);
        assert(b0->refcount_.load() == 2);
        assert(b1->refcount_.load() == 2);
        assert(b2->refcount_.load() == 2);

        e0.trigger();
        // trigger_member removes e0 from members_, quorum fires
        // locked_trigger removes Q from e1/e2 listener lists but keeps members_
        assert(b0->refcount_.load() == 1);
        assert(b1->refcount_.load() == 1);
        assert(b2->refcount_.load() == 1);
    }
    // Q freed → members_ dropped → e1, e2 released
    assert(b0->refcount_.load() == 1);
    assert(b1->refcount_.load() == 1);
    assert(b2->refcount_.load() == 1);

    std::cerr << "leak_any: ok\n";
}


// Leak test 2: nested any(all(), all()) frees entire non-triggering branch.
void test_leak_nested() {
    std::cerr << "=== leak_nested ===\n";

    cot::event e0, e1, e2, e3;
    auto* b0 = e0.handle().get();
    auto* b1 = e1.handle().get();
    auto* b2 = e2.handle().get();
    auto* b3 = e3.handle().get();

    {
        cot::event q = cot::any(cot::all(e0, e1), cot::all(e2, e3));
        // e0,e1 held by Q_all01; e2,e3 held by Q_all23
        assert(b0->refcount_.load() == 2);
        assert(b1->refcount_.load() == 2);
        assert(b2->refcount_.load() == 2);
        assert(b3->refcount_.load() == 2);

        // Trigger the first all() branch
        e0.trigger();
        // Q_all01: trigger_member removes e0, not at quorum yet
        assert(b0->refcount_.load() == 1);
        assert(b1->refcount_.load() == 2); // still in Q_all01.members_

        e1.trigger();
        // Q_all01: trigger_member removes e1, quorum reached → locked_trigger
        // Q_all01 fires → Q_any.trigger_member → quorum reached → Q_any fires
        // Q_all01 is freed (removed from Q_any.members_ by trigger_member,
        // extra ref from locked_trigger dropped)
        assert(b0->refcount_.load() == 1);
        assert(b1->refcount_.load() == 1);
        // But Q_all23 still held by Q_any.members_ (non-triggering branch)
        assert(b2->refcount_.load() == 1);
        assert(b3->refcount_.load() == 1);
    }
    // Q_any freed → Q_all23 freed, but it is kept alive in e2, e3
    assert(b2->refcount_.load() == 1);
    assert(b3->refcount_.load() == 1);

    std::cerr << "leak_nested: ok\n";
}


// Leak test 3: verify co_await any() releases promptly after loop().
void test_leak_await() {
    std::cerr << "=== leak_await ===\n";

    cot::event e0, e1;
    auto* b1 = e1.handle().get();

    auto fn = [&]() -> cot::task<> {
        co_await cot::any(e0, e1);
        // After resuming, the awaiter should be destroyed, dropping Q_any.
    };
    auto t = fn();

    assert(b1->refcount_.load() == 2); // user + Q_any.members_

    e0.trigger();
    // Q_any triggered, coroutine scheduled in ready_ queue
    // But awaiter not destroyed yet (coroutine hasn't resumed)
    assert(b1->refcount_.load() == 1);

    cot::loop();
    // Coroutine resumes → awaiter destroyed → Q_any freed → e1 released
    assert(t.done());
    assert(b1->refcount_.load() == 1);

    std::cerr << "leak_await: ok\n";
}


// Cross-thread task await: task created on thread A, co_awaited on thread B.
// When F completes on thread A, final_suspend should marshal the continuation
// (thread B's consumer coroutine) back to thread B's driver, not resume it
// directly on thread A.
void test_cross_thread_await_task() {
    std::cerr << "=== cross_thread_await_task ===\n";

    std::atomic<int> phase{0};
    cot::event* ev_ptr = nullptr;
    std::optional<cot::task<int>> f_task;
    std::thread::id thread_a_id, thread_b_id, resumed_on;

    std::thread thread_a([&] {
        thread_a_id = std::this_thread::get_id();
        cot::event ev;
        ev_ptr = &ev;

        auto fn = [&]() -> cot::task<int> {
            co_await ev;
            co_return 42;
        };
        f_task.emplace(fn());

        phase.store(1, std::memory_order_release);
        while (phase.load(std::memory_order_acquire) < 2) {
            std::this_thread::yield();
        }

        // Complete F on thread A
        ev.trigger();
        cot::loop();

        while (phase.load(std::memory_order_acquire) < 3) {
            std::this_thread::yield();
        }
    });

    std::thread thread_b([&] {
        thread_b_id = std::this_thread::get_id();
        while (phase.load(std::memory_order_acquire) < 1) {
            std::this_thread::yield();
        }

        auto consumer = [&]() -> cot::task<> {
            int val = co_await std::move(*f_task);
            resumed_on = std::this_thread::get_id();
            assert(val == 42);
        };
        auto c = consumer();

        phase.store(2, std::memory_order_release);

        // Consumer should be resumed here via remote_ready_, not on thread A
        while (!c.done()) {
            cot::loop();
            std::this_thread::yield();
        }

        phase.store(3, std::memory_order_release);
    });

    thread_a.join();
    thread_b.join();

    std::cerr << "  resumed on thread "
              << (resumed_on == thread_a_id ? "A" : resumed_on == thread_b_id ? "B" : "?")
              << " (expected B)\n";
    assert(resumed_on == thread_b_id && "consumer should resume on its home thread");

    std::cerr << "cross_thread_await_task: ok\n";
}


// Torture: Race between event_handle::~event_handle (destroy) and
// trigger_member on the same quorum. Thread A triggers member event `e`,
// Thread B drops the last explicit handle to the quorum. The dangerous
// interleaving: Thread B's fetch_sub reaches 0 and enters destroy(), but
// Thread A has already copied the quorum from `e`'s listener list and bumps
// refcount 0→1, triggers, releases (1→0), and deletes — then Thread B
// accesses freed memory in destroy().
void test_torture_destroy_trigger_race() {
    std::cerr << "=== torture_destroy_trigger_race ===\n";

    constexpr int ROUNDS = 5000;

    for (int round = 0; round < ROUNDS; ++round) {
        std::atomic<bool> go{false};
        cot::event e;
        // q holds the ONLY event_handle to the quorum (refcount=1).
        // any(e, after(1000h)) creates a quorum with quorum=1, two members.
        std::optional<cot::event> q(
            cot::any(e, cot::after(std::chrono::hours(1000))));

        std::thread thread_a([&] {
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            // Triggers e → trigger_unlock copies quorum from e's listener list,
            // bumps quorum refcount, calls trigger_member.
            e.trigger();
        });

        std::thread thread_b([&] {
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            // Drops the last explicit handle → refcount hits 0 → destroy().
            q.reset();
        });

        go.store(true, std::memory_order_release);

        thread_a.join();
        thread_b.join();
    }

    std::cerr << "torture_destroy_trigger_race: ok\n";
}


int main() {
    test_cross_thread_trigger();
    test_cross_thread_any();
    test_cross_thread_multi();
    test_independent_drivers();
    test_cross_thread_already_triggered();
    test_torture_hammer();
    test_torture_same_event();
    test_torture_quorum_race();
    test_torture_nested_quorum();
    test_torture_bidirectional();
    test_torture_inject_during_loop();
    test_torture_any_race();
    test_leak_any();
    test_leak_nested();
    test_leak_await();
    test_cross_thread_await_task();
    test_torture_destroy_trigger_race();
}
