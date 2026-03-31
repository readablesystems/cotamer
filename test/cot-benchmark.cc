#include "cotamer/cotamer.hh"
#include <chrono>
#include <cstring>
#include <iostream>
#include <print>
#include <vector>

// Many of the benchmarks in this file were written with, or by, Claude Code.
// Thanks, Claude Code!

namespace cot = cotamer;
using namespace std::chrono_literals;
using steady_clock = std::chrono::steady_clock;


// --- Benchmark infrastructure ---

struct benchmark_result {
    const char* name;
    double iterations_per_second;
    double ns_per_iteration;
    unsigned long iterations;
    double wall_seconds;
};

static std::vector<benchmark_result> results;

// Run `fn` repeatedly for at least `min_time` wall-clock seconds.
// `fn` returns a task<> that is the benchmark body for one iteration.
template <typename F>
void bench(const char* name, F fn, double min_time = 1.0) {
    // Warmup: run a few iterations to stabilize caches/branch predictors.
    for (int i = 0; i < 10; ++i) {
        cot::reset();
        auto t = fn();
        cot::loop();
    }

    // Measure: run iterations in growing batches until min_time elapsed.
    unsigned long total_iters = 0;
    double total_wall = 0.0;
    unsigned long batch = 100;

    while (total_wall < min_time) {
        cot::reset();
        auto start = steady_clock::now();
        auto t = [&]() -> cot::task<> {
            for (unsigned long i = 0; i < batch; ++i) {
                co_await fn();
            }
        }();
        cot::loop();
        auto end = steady_clock::now();
        double elapsed = std::chrono::duration<double>(end - start).count();
        total_iters += batch;
        total_wall += elapsed;

        // Scale batch to try to finish in ~0.5s next round.
        if (elapsed > 0.0) {
            unsigned long target = static_cast<unsigned long>(
                batch * 0.5 / elapsed);
            batch = std::max(target, batch);
        }
    }

    double ns_per_iter = (total_wall * 1e9) / total_iters;
    double iters_per_sec = total_iters / total_wall;

    results.push_back({name, iters_per_sec, ns_per_iter,
                        total_iters, total_wall});
    std::print(std::cerr, "{:40s}  {:12.0f} iter/s  {:10.1f} ns/iter  "
               "({} iters in {:.2f}s)\n",
               name, iters_per_sec, ns_per_iter,
               total_iters, total_wall);
}


// --- Benchmarks ---

// Benchmark: race() with 2 tasks, one fast and one slow.
cot::task<> bench_race_2() {
    auto fast = []() -> cot::task<int> {
        co_await cot::after(1h);
        co_return 1;
    };
    auto slow = []() -> cot::task<int> {
        co_await cot::after(10h);
        co_return 2;
    };
    auto r = co_await cot::race(fast(), slow());
    (void) r;
}

// Benchmark: race() with 5 tasks.
cot::task<> bench_race_5() {
    auto mk = [](auto dur) -> cot::task<int> {
        co_await cot::after(dur);
        co_return 0;
    };
    auto r = co_await cot::race(
        mk(1h), mk(3h), mk(5h), mk(7h), mk(9h));
    (void) r;
}

// Benchmark: race() with 10 tasks.
cot::task<> bench_race_10() {
    auto mk = [](auto dur) -> cot::task<int> {
        co_await cot::after(dur);
        co_return 0;
    };
    auto r = co_await cot::race(
        mk(1h), mk(2h), mk(3h), mk(4h), mk(5h),
        mk(6h), mk(7h), mk(8h), mk(9h), mk(10h));
    (void) r;
}

// Benchmark: race() where the winner completes immediately.
cot::task<> bench_race_immediate() {
    auto imm = []() -> cot::task<int> { co_return 42; };
    auto slow = []() -> cot::task<int> {
        co_await cot::after(10h);
        co_return 0;
    };
    auto r = co_await cot::race(imm(), slow(), slow(), slow());
    (void) r;
}

// Benchmark: many sequential races (measures overhead of repeated setup/teardown).
cot::task<> bench_race_sequential_100() {
    auto fast = []() -> cot::task<int> {
        co_await cot::after(1h);
        co_return 1;
    };
    auto slow = []() -> cot::task<int> {
        co_await cot::after(10h);
        co_return 2;
    };
    for (int i = 0; i < 100; ++i) {
        auto r = co_await cot::race(fast(), slow());
        (void) r;
    }
}

// Benchmark: race() with void tasks.
cot::task<> bench_race_void() {
    auto fast = []() -> cot::task<> {
        co_await cot::after(1h);
    };
    auto slow = []() -> cot::task<> {
        co_await cot::after(10h);
    };
    co_await cot::race(fast(), slow(), slow());
}

// Benchmark: race() between a task and an event.
cot::task<> bench_race_task_vs_event() {
    auto slow = []() -> cot::task<> {
        co_await cot::after(10h);
    };
    co_await cot::race(slow(), cot::after(1h));
}


// --- Main ---

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
        bench(name, fn);
    };

#define RUN_BENCH(name) run(#name, bench_##name)
    RUN_BENCH(race_2);
    RUN_BENCH(race_5);
    RUN_BENCH(race_10);
    RUN_BENCH(race_immediate);
    RUN_BENCH(race_sequential_100);
    RUN_BENCH(race_void);
    RUN_BENCH(race_task_vs_event);

    if (ran == 0) {
        std::print(std::cerr, "No matching benchmarks\n");
        exit(1);
    }
    exit(0);
}
